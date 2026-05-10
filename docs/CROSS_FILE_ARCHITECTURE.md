# Cross-File LSP Resolution Architecture

**Goal:** Wire LSP-augmented type-aware call resolution across files in the
indexing pipeline, while keeping the latency overhead **under +10% of total
indexing time** (vs the conservative +30% naive estimate).

This document is the design output of plan-task #1; implementation lands in
plan-tasks #2 (hash-table registry) and #6 (pipeline wiring).

---

## 1. Current state (post-iter 37)

### Single-file flow (works today)

```
cbm_extract_file(source, lang, project, rel_path, ...)
  ├── tree-sitter parse → TSTree
  ├── cbm_extract_definitions(ctx)         // result->defs filled
  ├── cbm_extract_imports(ctx)
  ├── cbm_extract_unified(ctx)              // tree-sitter call edges
  └── if lang in {TS, JS, TSX, JAVASCRIPT}:
      └── cbm_run_ts_lsp(arena, result, source, len, root, modes...)
          ├── cbm_registry_init(reg) — local to this call
          ├── cbm_ts_stdlib_register(reg)
          ├── register_file_defs(reg, result->defs)        // file-only
          ├── ts_lsp_init(ctx, ...)
          ├── ast_sweep_shapes(ctx, root, reg)
          ├── rebuild_signatures_from_ast(ctx, root, reg)
          ├── convert_signature_type_params(ctx, root, reg)
          ├── apply_jsdoc_signatures(ctx, root, reg)
          ├── infer_implicit_returns(ctx, root, reg)
          └── ts_lsp_process_file(ctx, root)               // emits resolved_calls
```

**Critical limitation:** `register_file_defs` only sees this file's defs.
Cross-file types (`import { Foo } from './bar'` where `Foo` is defined in `bar.ts`)
are NOT in the registry. So `c.method()` where `c: Foo` resolves to UNKNOWN
because the resolver doesn't know about Foo's structure.

### Cross-file path (exists but pipeline-unwired)

```
cbm_run_ts_lsp_cross(arena, source, ..., defs, def_count, imports, ..., out)
```

Takes a manually-constructed `CBMLSPDef[]` array. The 4 cross-file tests
build this array in C code. Pipeline doesn't call this.

### Latency profile today

For a 1M-LOC TS project (~3000 files):
- Parse + extract: ~12 sec
- Single-file LSP: ~3 sec
- **Total: ~15 sec**

---

## 2. Naive cross-file approach (and why we reject it)

```
Pass 1: extract every file (cbm_extract_file)              [~12 sec]
Pass 2: build project-wide def-map from all results        [~0.5 sec]
Pass 3: re-run LSP per file with project-wide map          [~3 sec ?]
        — but each per-file LSP invocation re-parses, re-extracts shapes,
          re-builds the registry from scratch
        — 3000 files × ~5ms registry rebuild = ~15 sec ALONE
        — plus per-call O(n) lookups against the registry
Total: ~30 sec (+100% over today)
```

This is the +30% estimate I gave previously, and it's actually worse on closer
inspection. Two specific waste points:

1. **Registry rebuilt N times** — every file builds its own
   `CBMTypeRegistry`, even though most contents are project-wide stdlib +
   shared cross-file defs.
2. **Linear-scan registry lookups** — `cbm_registry_lookup_method` is O(n) on
   the func array. With ~5000 stdlib funcs (post-generator) + ~10000 user funcs
   (1M-LOC project), every call resolution is ~15000 strcmps. ~5M call
   resolutions × ~15us = ~75 sec **just on lookups**.

So the naive approach would actually be a 5–10× regression on real projects.

---

## 3. Target architecture (this design)

### Three-pass model with shared registry

```
Pass 1: cbm_pipeline_pass_definitions     [unchanged, ~12 sec]
   ↓ each file's CBMFileResult contains its defs[]

Pass 1.5 (NEW): build project-wide registry              [~0.5 sec]
   - Walk all files' result->defs
   - Insert into single hash-table-indexed CBMTypeRegistry
   - Stdlib registered once at the start (not per-file)

Pass 2: cbm_pipeline_pass_calls            [unchanged tree-sitter calls]

Pass 2.5 (NEW): cbm_pipeline_pass_lsp_resolve            [~2 sec target]
   - Per file (parallel via worker pool):
     * Reuse cached_tree (no re-parse)
     * Reuse cached defs from pass 1
     * Open a TSLSPContext pointing at the SHARED project-wide registry
     * Run ts_lsp_process_file()
     * Emit resolved_calls into the file's CBMFileResult
   - No registry rebuild per file. No re-extract. Just walk + resolve.

Pass 3+: existing usage/semantic/etc.       [unchanged]
```

### Key invariants

1. **One registry per project, not per file.** Built once after pass 1
   completes; lives for the duration of the indexing run.
2. **Hash-table-indexed.** Adding a stdlib of 5000 funcs costs the same
   per-lookup as adding 50.
3. **Cached AST trees reused.** `cbm_run_ts_lsp` already reads
   `result->cached_tree`; cross-file pass does the same.
4. **Per-file resolution is independent.** Files can resolve in parallel
   via the existing `cbm_worker_pool_dispatch` mechanism.
5. **Read-only registry during resolution.** All writes happen in pass 1.5;
   pass 2.5 readers don't lock.

---

## 4. Hash-table registry (task #2)

### Why O(1) is essential

| Item | Per-lookup cost (linear scan) | Per-lookup cost (hash) |
|---|---|---|
| Today's stdlib (~150 funcs) | ~1 µs | ~50 ns |
| Project funcs (~10K user funcs) | ~50 µs | ~50 ns |
| After stdlib generator (~5K stdlib + 10K project) | ~75 µs | ~50 ns |

For a 1M-LOC project with ~5M method-call resolutions:
- Linear scan: 5M × 75 µs = **375 sec** in lookups alone
- Hash: 5M × 50 ns = **0.25 sec**

### Data structure

```c
typedef struct CBMRegistryHashEntry {
    uint64_t hash;          // FNV-1a or xxhash of (qualified_name)
    int func_index;         // -1 if type, else index into reg->funcs[]
    int type_index;         // -1 if func, else index into reg->types[]
    struct CBMRegistryHashEntry *next;  // chain for receiver+name overload list
} CBMRegistryHashEntry;

typedef struct CBMTypeRegistry {
    CBMRegisteredFunc* funcs;
    int func_count, func_cap;
    CBMRegisteredType* types;
    int type_count, type_cap;
    CBMArena* arena;

    // NEW
    CBMRegistryHashEntry **func_buckets;   // by qualified_name
    int func_bucket_count;
    CBMRegistryHashEntry **type_buckets;   // by qualified_name
    int type_bucket_count;
    // Methods grouped by (receiver_type, short_name) for overload sets
    CBMRegistryHashEntry **method_buckets;
    int method_bucket_count;
} CBMTypeRegistry;
```

### Hash function

Use xxHash (already vendored at `internal/cbm/vendored/xxhash`). Single call
per lookup: `XXH64(qualified_name, strlen, 0)`. Fast on modern CPUs (~10
GB/s).

### Resize policy

- Initial bucket count: 1024 (handles small projects)
- Grow at 75% load
- Grow factor: 2× (rounded to next power of two)

### Linear-scan retained for

- Iteration code paths (e.g., "list all methods of type X")
- Pattern-match queries (e.g., "find all funcs with name matching regex")

These are rare enough that O(n) is acceptable.

### API stays the same

`cbm_registry_lookup_method`, `cbm_registry_lookup_func`, etc. all keep their
signatures. Internal implementation switches to hash-table probe.

---

## 5. Parallelism model

### Pass 1.5 (registry build)

Sequential — but tiny (~0.5 sec). Each file's defs are in its `CBMFileResult`
already; just walk and insert.

If sequential is a bottleneck on >10M LOC projects, sharded build with
per-thread sub-registries merged at the end is bounded extra work (~150 LOC).

### Pass 2.5 (resolution per file)

Parallel via worker pool. Each worker:
- Holds a thread-local `TSLSPContext`
- Reads from the shared project-wide registry (read-only after pass 1.5)
- Writes to its own file's `CBMFileResult.resolved_calls` (no contention)

Existing precedent: `cbm_pipeline_pass_calls` already runs file-parallel via
`cbm_worker_pool_dispatch`. Same pattern reused.

---

## 6. Memory budget

| Component | Single-file (today) | Cross-file (target) |
|---|---|---|
| Per-file arena | ~1 MB avg | ~1 MB avg (unchanged) |
| Per-file registry | ~50 KB | gone (shared) |
| Project-wide registry | n/a | ~2 MB stdlib + ~10 MB user defs (1M-LOC project) |
| Hash buckets | n/a | ~200 KB total |

**Net memory delta: +12 MB on a 1M-LOC project.** Negligible relative to the
existing ~500 MB peak the pipeline uses.

---

## 7. Latency budget

| Operation | Today | Naive cross-file | Target architecture |
|---|---|---|---|
| Parse + extract | 12.0 sec | 12.0 sec | 12.0 sec |
| Per-file LSP (single-file) | 3.0 sec | n/a | n/a |
| Pass 1.5 build registry | 0.0 sec | 0.5 sec | 0.5 sec |
| Pass 2.5 LSP resolve | n/a | 18.0 sec | 2.0 sec |
| **Total** | **15.0 sec** | **30.5 sec** | **14.5 sec** |
| **Delta** | baseline | +103% | **−3% (parallelism wins back)** |

Negative delta is plausible because pass 2.5 is more parallelizable than
single-file LSP (which runs interleaved with extraction today). Realistic
target: **+0% to +10%**.

### Latency gates (rejection criteria)

- Total indexing time on TS bench corpus increases by >25%: revert.
- Memory peak increases by >10%: revert.
- Resolution coverage doesn't increase by ≥+5%: question whether the
  cross-file pass is doing useful work.

---

## 8. Backward compatibility

- `cbm_run_ts_lsp` keeps its existing signature (single-file mode).
- `cbm_run_ts_lsp_cross` keeps its existing signature (manual-defs mode for
  tests).
- New entry: `cbm_pipeline_pass_lsp_resolve(ctx, files, count, registry)` —
  internal pipeline-only.
- Existing 268 tests must pass unchanged.
- `CBM_LSP_DISABLED=1` env knob still respected (skip pass 2.5 entirely).

---

## 9. Failure modes & mitigations

| Failure | Probability | Mitigation |
|---|---|---|
| Hash collision storm (adversarial QN strings) | Very low | xxHash mixes well; no security risk |
| Registry growth blows memory cap | Low | Estimate up-front from def_count × avg_size; pre-size buckets |
| Cycle in re-export chain (after task #10) | Medium | Cycle guard at 16 hops, same as alias chain |
| Stale cached_tree after edit (incremental indexing) | Medium | Existing `cbm_free_tree` invalidation chain handles this |
| Worker pool starvation on small projects (<10 files) | Low | Fall back to sequential when file_count < worker_thread_count × 4 |

---

## 10. Implementation order (per plan task IDs)

1. **Task #1 (this document)** — design, no code.
2. **Task #2** — hash-table registry. Standalone; doesn't change behavior,
   just data structure. Validates with existing 268 tests.
3. **Task #6** — pass_lsp_resolve pipeline wiring. Uses #2's hash registry.
   First user-visible benefit.
4. **Tasks #3, #4, #5, #7, #8, #9, #10, #11** — each independently builds on
   the foundation.
5. **Task #12** — final benchmark.

---

## 11. Out of scope for this design

- **Incremental indexing.** Reusing the cross-file registry across watch-mode
  re-indexes is a follow-up — current scope is full-project indexing.
- **Distributed/federated registries.** All-in-process for now.
- **Cross-language LSP coordination.** Each language LSP gets its own
  registry; no shared state across Go/C/C++/TS LSPs.
- **Memory-mapped registry persistence.** The registry is fully in-RAM and
  rebuilt every indexing run.
