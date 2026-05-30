# Codebase-Memory-MCP — Improvement Plan (speed + accuracy)

**Status:** revised after independent review (Codex gpt-5.5, xhigh) on 2026-05-28.
**Scope:** improve indexing speed and call/type-resolution accuracy, emphasis on Go, C++, Python.
**Basis:** code-level study of the indexing pipeline + a skeptical code-grounded review whose corrections are folded in below.

---

## 0. What the review changed (so reviewers can diff intent)

The review **confirmed** the two headline problems (WS1 serial per-file registry rebuild; WS2 unplugged `compile_commands.json`) but corrected several specifics. Folded in:
- WS1 split into a safe-parallelize phase and a harder shared-registry phase, because the per-file registries are **mutated during analysis** — a plain read-only share is unsafe.
- WS0 metrics rewritten: measure at **resolution time** (pre-dedup/insert), add callsite IDs for ground truth, use the **real** strategy labels and `STARTS WITH` (not `LIKE`); `fuzzy` is *not* a production strategy.
- WS2 corrected: only the **second** (preprocessed) C/C++ parse consumes flags, `-std` is currently hardcoded, sequential/fallback call sites also pass NULL, and the C cross-LSP "include map" is not a compiler search path (that sub-item dropped).
- New workstream: **incremental indexing skips cross-file LSP entirely** — correctness gap.
- Go generics gap narrowed; Go-stdlib regeneration not assumed reproducible (generator not found).

---

## 1. Current architecture (one-paragraph recap)

Indexing runs as a multi-phase pipeline (`src/pipeline/pipeline.c::cbm_pipeline_run`):
discover → structure nodes → **parallel extraction** (per-worker tree-sitter parse + def-node creation into per-worker graph buffers, merged sequentially) → registry build + DEFINES/IMPORTS edges → **cross-file LSP resolution** → **parallel call/usage resolution** → predump enrichment (similarity, semantic, git, routes) → id-remap → SQLite dump. Resolution is two-tier: hand-written in-process per-language type resolvers (`internal/cbm/lsp/{c,go,py,ts,php,cs}_lsp.c`, *not* real language servers) + a confidence-scored name registry (`src/pipeline/registry.c`). Similarity = structural MinHash + LSH; semantic edges = vendored static token embeddings + LSH (no transformer at index time).

The architecture is sound. The biggest wins are in stages that **already exist but are under-built, unplugged, or skipped on the incremental path** — not redesigns.

---

## 2. Goals / non-goals

**Goals**
- Cut end-to-end index time on large Go/Python/C++ repos (cross-file phase should stop dominating; target ≥2× on large repos).
- Raise `resolved / total` CALLS ratio and lower false-positive CALLS edges for Go, C++, Python — **measured**, not asserted.
- Make accuracy measurable per language with callsite-level ground truth.

**Non-goals**
- Compiler-grade soundness (SCIP/Kythe/CodeQL). We stay heuristic + build-free.
- Dynamic-dispatch cases declared out of scope in `docs/BENCHMARK_PYTHON.md` (getattr, metaclasses, monkeypatching, eval/exec).
- Rewriting the per-language resolvers; we extend and de-duplicate them.

---

## 3. WS0 — Measurement foundation (P0, prerequisite)

Today `docs/BENCHMARK.md` is a *capability* benchmark (Q1–Q12); C++ at 100% there is discovery on header-only `nlohmann/json`, not call-graph precision. Resolution-ratio harnesses exist for **Python** (`tests/test_py_lsp_bench.c`) *and* **C#** (`tests/test_cs_lsp_bench.c`) — but both report `resolved/total` ratios, **not** precision against callsite ground truth.

**Why ratio-from-the-DB is not enough (review finding):** unresolved calls are dropped during resolution, and persisted edges are **deduped by `(source_id, target_id, type)`** (`src/graph_buffer/graph_buffer.c:875`), and `CBMCall` has **no stable callsite id/line/column** (`internal/cbm/cbm.h:216`). So you cannot recover per-callsite precision/recall from the graph alone.

**Work items**
1. **Instrument at resolution time, before dedup/insert:** count textual extracted calls, resolved calls, and the **strategy/confidence histogram** straight from the resolver output (not the SQLite edges).
2. **Add a stable callsite id** (file + byte offset or line:col) to `CBMCall`/resolved-call records so a fixed sample can be hand-labeled and re-measured across commits.
3. **Use the real labels.** Production strategies are `import_map`, `same_module`, `unique_name`, `suffix_match` (`src/pipeline/registry.c:320`) plus the `lsp_*` precise-tier labels; **`fuzzy` is not emitted by the normal pipeline.** Cypher uses `STARTS WITH 'lsp_'` (the custom engine has no `LIKE`).
4. **Per-language in-process benches** for Go and C++ (`tests/test_{go,c}_lsp_bench.c`) mirroring the Python/C# ones, reporting precise-tier vs heuristic-tier split.
5. **Repo-level harness** (extend `scripts/benchmark-index.sh`): per-phase `CBM_PROF` timings (add a timer for the cross phase), resolved-ratio, and a random N=50 callsite sample for manual false-positive review.
6. **Baselines** committed to `docs/` on 3 corpora before any change: a large Go repo, Django (Python), and a C++ repo *with* `compile_commands.json`.

**Exit criteria:** committed baselines (timings + resolution ratio + strategy histogram + a labeled callsite sample) so every later WS shows a real delta.

---

## 4. Workstreams

Priority: **P0** do-first, **P1** high value, **P2** opportunistic.

### WS1 — Fix the cross-file LSP pass (P0, speed) — split into 1A (safe) and 1B (registry)

**Problem (confirmed).** `src/pipeline/pass_lsp_cross.c::cbm_pipeline_pass_lsp_cross` (loop at `:427`) is **serial**, and each per-language cross entry rebuilds a fresh `CBMTypeRegistry`, **re-registers the full stdlib, and re-registers all project defs, per file**: Python `internal/cbm/lsp/py_lsp.c:3113`, Go `go_lsp.c:2477`, C `c_lsp.c:4572`. So the phase is ≈ `O(N_files × (parse + full_stdlib + total_project_defs))`. The `pxc_run_one` scratch arena already fixed the *memory* blowup (3.5 GB on a 1100-file repo); the **CPU redundancy remains** and likely dominates large Go/Python indexes (WS0 confirms before we commit to this).

**WS1A — Parallelize the file loop (lower risk, do first).**
- Wrap the per-file loop with `src/pipeline/worker_pool.c::cbm_parallel_for` + per-worker output buffers, mirroring `cbm_parallel_resolve`'s proven dispatch/merge. Each worker keeps its **own** registry today (no sharing yet), so correctness is unchanged; we just stop running files one at a time.
- Confirm concurrent gbuf **reads** in `pxc_build_import_map` are safe (no writer runs in this phase) and TSan-clean.

**WS1B — Immutable base registry + per-file overlay (higher risk, the real speedup).**
The review shows a plain "build once, share read-only" is **unsafe**: the cross passes **mutate** the registry during per-file analysis — Go patches aliases/fields/type-params (`go_lsp.c:2541`), C casts away `const` and adds funcs/signatures (`c_lsp.c:4572`), TS sweeps add/patch types/funcs (`ts_lsp.c:4415`), PHP mutates class fields (`php_lsp.c:3885`), Python adds instance fields.
- Design: build the **stdlib + project-def base registry once**, then give each file a **mutable overlay (or cheap clone)** for its analysis-time additions. Resolution checks overlay → base.
- Respect the registry's existing **`finalize`/hash-index** mechanism (`internal/cbm/lsp/type_registry.h:65`, `type_registry.c:107`) — production currently doesn't finalize; adding entries after finalize bypasses the hash index. The base must be finalized; overlays must index their own additions.
- **Per-language mutation audit** is an explicit deliverable: enumerate every registry write in each `*_lsp_cross` and route it to the overlay.

**WS1C — De-lossify the projection (unblocks 1B's value).**
`pxc_build_lsp_def` (`src/pipeline/pass_lsp_cross.c:126`) only carries name/receiver/return/base-class and **skips field labels via `pxc_map_label`** — no field defs, method-name strings, decorators, or structured return arrays. A prebuilt base registry is only as good as this projection. Enrich `CBMLSPDef` to carry fields/methods/signatures/decorators where the extractor already has them.

**Effort:** 1A=S, 1B=L, 1C=M. **Risk:** 1B is the real risk (overlay correctness + finalize semantics + per-language mutation coverage) — gate behind the WS0 baseline and TSan. **Validation:** WS0 timing delta on Django + large Go; resolution ratio must not regress.

---

### WS2 — Wire `compile_commands.json` into C/C++ extraction (P0, accuracy: C++) — do first after WS0

**Problem (confirmed).** `src/pipeline/pass_compile_commands.c` (`cbm_parse_compile_commands`, `cbm_extract_flags`) is **test-only**; the parallel worker calls `cbm_extract_file(..., NULL, NULL)` (`src/pipeline/pass_parallel.c:486`). So preprocessing runs with no project defines/includes.

**Corrections from review (must be in the design):**
- Flags only affect the **second** C/C++ preprocessed parse (`internal/cbm/cbm.c:392`); the **first** parse is raw source. Decide whether to also feed flags into the first parse or accept that defs come from the raw parse and only macro-hidden calls come from the preprocessed pass.
- `internal/cbm/preprocessor.cpp:63` **hardcodes C11/C++17** — the parsed `-std=` is currently ignored. Either thread `-std` through or document that it's unused.
- **All** extraction call sites pass NULL, not just `extract_worker`: also sequential/fallback paths `pass_definitions.c:354`, `pass_calls.c:380`, `pass_semantic.c:376`. Wire them too or they silently diverge.
- **Dropped (refuted):** feeding compile include dirs into `cbm_run_c_lsp_cross` is *not* the same thing — `CLSPContext`'s include map is header-path→namespace-QN-prefix (`internal/cbm/lsp/c_lsp.h:18`), not a compiler search path, and isn't used for resolution beyond insertion. Don't conflate them.

**Work items.**
1. Parse `compile_commands.json` once at pipeline start; store a canonicalized `file → cbm_compile_flags_t` map on `cbm_pipeline_ctx_t`.
2. Thread it through `extract_ctx_t` → `extract_worker` → `cbm_extract_file` **and** the sequential/fallback call sites above.
3. Thread `-std` into `cbm_preprocess` (remove the hardcode) or document.
4. Harden `cbm_extract_flags`: it sizes arrays to `argc` **without a guaranteed NULL terminator** (`pass_compile_commands.c:125`) while `cbm_preprocess` expects NULL-terminated arrays — fix this; add response-file (`@file`) handling, more include-flag forms (`-I`, `-isystem`, `-iquote`), duplicate-entry policy, and path canonicalization. Cap counts; sandbox include traversal.
5. Graceful fallback when absent (today's behavior).

**Effort:** M. **Risk:** low–medium (NULL-termination bug is a real crash risk — fix first). **Validation:** WS0 C++ corpus *with* `compile_commands.json`: expect higher resolution ratio and fewer phantom defs from mis-expanded macros.

---

### WS3 — Cross-phase tree reuse (P1, speed; complements WS1)

**Problem (corrected).** The plan previously claimed universal re-parse. Accurate picture: the **parallel** path frees each tree in-worker (`src/pipeline/pass_parallel.c:507`) so cross-LSP sees `cached_tree == NULL` and re-parses; but `cbm_extract_file` **does** retain the tree (`internal/cbm/cbm.c:461`) and the **sequential** definitions path can keep `result_cache` + trees (`pass_definitions.c:377`). So tree reuse is partially plumbed already.

**Options (after WS1 + WS0 show parse time matters):**
- **(a) Bounded tree cache** under `cbm_mem_budget`, passed via the existing `cached_tree` param. **Risk the plan must own:** trees are freed in-worker partly for RSS *and* tree-sitter allocator/slab/thread-ownership reasons (`cbm_destroy_thread_parser`/`cbm_slab_reclaim` between files). A cache must keep the owning parser/slab alive or detach trees safely — not just retain `TSTree*`.
- **(b) Single-pass resolution:** build a project symbol/type index in a cheap first pass, then resolve intra+cross in one live-tree walk. Larger, eliminates the second parse.

**Recommendation:** (a) first if WS0 shows parse cost in the cross phase; consider (b) later. **Effort:** S/L. **Validation:** cross-phase parse time → ~0 in WS0 profile; RSS stays bounded.

---

### WS4 — Confidence-banded / ambiguity-aware edges (P1, accuracy)

**Problem.** When the precise tier can't disambiguate (C++ overloads/templates, multi-candidate names), the registry picks one or drops it. The registry computes `confidence` + `strategy` (`registry.c`).

**Work items.**
1. Persist `confidence` and `strategy` on every CALLS edge; verify they survive gbuf→SQLite and are queryable. **Use the real strategy names** (`import_map`/`same_module`/`unique_name`/`suffix_match` + `lsp_*`).
2. For multi-candidate cases emit the chosen edge at low confidence **plus** a `candidates` property, instead of dropping/silently guessing.
3. Expose a confidence threshold in the query tools so consumers can request high-confidence-only call graphs.

**Effort:** S–M. **Risk:** low (edge dedup by `(src,tgt,type)` means properties may merge — define merge policy). **Validation:** WS0 false-positive rate by band; high band <5% FP.

---

### WS5 — Go: narrow generics gap + package coverage (P1, accuracy: Go)

**Problem (narrowed by review).** Go stdlib tables exist (`internal/cbm/lsp/generated/go_stdlib_data.c`, 34 packages). Go *does* perform generic **function** type substitution/inference (`go_lsp.c:414`). The actual gap: generic **type instantiations are collapsed to the base named type** (`go_lsp.c:221`), so methods reached through an instantiated generic type can mis-resolve. Plus partial stdlib package coverage and unresolved third-party modules.

**Work items.**
1. Carry type arguments on instantiated generic types in `go_eval_expr_type`/`go_lookup_field_or_method_depth` instead of collapsing at `go_lsp.c:221`.
2. Expand `go_stdlib_data.c` coverage by call frequency from WS0. **Note:** the generator script wasn't located in the repo — locate or recreate it before assuming regeneration is reproducible.
3. Verify multi-return inference (`a, b := f()`).

**Effort:** M/S. **Risk:** medium (generic instantiation). **Validation:** `test_go_lsp_bench.c` on a generics-heavy repo.

---

### WS6 — Python: cross-module return/field type propagation (P1, accuracy: Python)

**Problem.** Intra-file typing is strong (`py_eval_expr_type`); cross-file fluent chains break when intermediate types live in another module. Tied to WS1C — the lossy `CBMLSPDef` projection is what starves cross-file lookups.

**Work items.**
1. Ensure WS1C populates return/field types for cross-file defs so `py_lookup_field_depth`/`py_lookup_attribute_depth` resolve across modules.
2. Expand `cbm_python_stdlib_register` coverage of the most-called `typing`/stdlib constructs (the ≥70% target in `docs/BENCHMARK_PYTHON.md` depends on stdlib).

**Effort:** M. **Risk:** low–medium. **Validation:** Django resolution ratio + cross-module chain spot-checks.

---

### WS7 — Incremental indexing parity (P1, accuracy/correctness) — NEW (review finding)

**Problem.** `src/pipeline/pipeline_incremental.c` runs extraction, registry build, and resolution but **does not run `cbm_pipeline_pass_lsp_cross`** (around `:393`). So incrementally re-indexed files get worse cross-file resolution than a full index — a silent correctness/consistency gap, and it interacts with WS1 (whatever we speed up only helps full indexes until this is fixed).

**Work items.**
1. Run cross-file LSP on the incremental path (scoped to changed files + their importers), or
2. explicitly document the behavioral difference and surface it in `index_status`.
- Couple with WS1: a parallel, registry-once cross pass is cheap enough to run incrementally over the affected set.

**Effort:** M. **Risk:** medium (need the changed-file → dependent-file set). **Validation:** full vs incremental resolution ratio on the same repo should converge.

---

### WS8 — Opportunistic speed (P2)

- **Embedding gating:** `pass_semantic_edges` already gates (`PSE_MIN_FUNCS_FOR_PAIR`) and uses static token vectors + LSH; skip vendored/generated/min-size functions; confirm tokenization is the bound.
- **Parser teardown cadence:** `extract_worker` destroys parser + reclaims slab between *every* file for RSS; amortize to every N files / RSS watermark (verify peak RSS stays bounded — see WS3 ownership note).
- **Content-hash incremental:** `classify_files` uses mtime+size; add optional xxhash (vendored) verification on the changed set to skip touched-but-identical files.

**Effort:** S each. **Validation:** WS0 timings + `scripts/soak-test.sh`.

---

### WS9 — Resolver maintainability guardrails (P2)

Correctness lives in very large hand-written resolvers (`c_resolve_calls_in_node` cyclomatic 151, `py_eval_expr_type` 146, `go_eval_expr_type` 122); recent branch fixes cluster here. Grow per-resolver golden-corpus tests (extend `test_{c,go,py,ts}_lsp.c`) so each accuracy fix ships with a regression test; track per-resolver complexity in CI.

---

## 5. Sequencing / milestones

| Milestone | Contents | Rationale |
|---|---|---|
| **M1 — Measure** | WS0 | Baselines + callsite IDs gate every accuracy/speed claim. |
| **M2 — Safe wins** | WS2 (compile_commands), WS1A (parallelize cross loop) | Lower risk; immediate C++ accuracy + Go/Py speed. |
| **M3 — Structural** | WS1B+1C (shared base registry + richer projection), WS6 (Py cross-module), WS7 (incremental parity) | The deep speed+accuracy work; depends on M1/M2. |
| **M4 — Depth + polish** | WS3 (tree reuse), WS4 (confidence bands), WS5 (Go generics), WS8, WS9 | Lower-frequency but real. |

Each WS ships behind existing mode flags (`fast`/`moderate`/`full`) and is validated against WS0 baselines.

---

## 6. Risks & mitigations

- **Shared registry mutation (WS1B):** per-file mutable overlay/clone over a finalized immutable base; explicit per-language mutation audit; TSan in CI.
- **`finalize`/hash-index semantics (WS1B):** base must be finalized; overlays index their own adds; never add to a finalized base.
- **compile_commands robustness (WS2):** fix the missing NULL terminator first; reuse `cbm_split_command` quoting; response files, include-flag variants, dup policy, path canonicalization; clean fallback.
- **Tree-cache ownership (WS3a):** keep parser/slab alive or detach trees safely; bound by `cbm_mem_budget`.
- **Metric gaming (WS0/WS4):** keep the manual callsite FP sample so resolution-ratio gains can't come purely from low-confidence guesses.
- **Incremental scope (WS7):** correct changed-file → importer set, or document the gap.

---

## 7. Open questions

1. Is the cross-file phase actually top-line cost on large repos vs parse/embedding? (WS0 answers; WS1 assumes it.)
2. WS3: bounded tree cache vs committing to single-pass resolution?
3. WS2: thread `-std` through, and do we feed flags into the first parse or only the preprocessed pass?
4. WS4: do confidence bands change default query behavior or stay opt-in? How do edge-dedup property merges interact with banding?
5. WS7: scope incremental cross-LSP to changed-files-plus-importers, or run it project-wide on any change?

---

## 8. Implementation status (this branch: `feat/indexing-accuracy-speed`)

**Implemented + builds clean:**
- **WS2 bug — `cbm_extract_flags` NULL terminator.** `include_paths`/`defines` now `calloc(argc+1)` so the arrays `cbm_preprocess` iterates are always NULL-terminated (`src/pipeline/pass_compile_commands.c`).
- **WS2 wiring — `compile_commands.json` → preprocessor.** New opaque `cbm_cc_index` (build/lookup/free in `pass_compile_commands.c`): parsed once in `cbm_pipeline_run`, stored on `cbm_pipeline_ctx_t.cc_index`, freed in cleanup. Threaded to **every** production `cbm_extract_file` call site — parallel `extract_worker` (via `extract_ctx_t.cc_index`) and the sequential `pass_definitions` / `pass_calls` / `pass_semantic` paths. Non-C/C++ files (and repos without the file) look up NULL and behave exactly as before.
- **WS7 — incremental cross-file LSP.** *(SUPERSEDED — reverted after self-review; see §9.)*

**Deliberately deferred (with rationale):**
- **WS2 `-std` propagation.** `-std=` is parsed into `cbm_compile_flags_t.standard` but **not yet applied** — `cbm_preprocess` still hardcodes `c11`/`c++17` (`preprocessor.cpp:63`). Threading it requires changing the `cbm_preprocess` (and `cbm_extract_file`) signatures, which ripples to ~30 callers incl. tests. Low marginal value; documented per the review's "thread OR document" guidance.
- **WS1A parallelize cross-LSP loop.** Not done: the cross resolvers re-parse via `ts_parser_new`, and the extraction path manages a **thread-local slab allocator** (`cbm_slab_install`/`cbm_destroy_thread_parser`) that the generic `cbm_parallel_for` worker threads do not set up. Parallelizing safely needs per-worker slab init (or confirming tree-sitter uses plain `malloc` here) — must be done with TSan, not blind.
- **WS1B / WS0 / WS3 / WS4 / WS5 / WS6.** Multi-day features (shared immutable-base registry + per-file overlay; callsite-ID precision harness; tree cache; confidence-banded edges; Go generic instantiation; Python cross-module type propagation). Left as designed.

**Known limitations of the WS2 wiring (first cut):**
- Path matching now lexically normalizes (`.`/`..`/`//`) on the compile_commands side (§9); symlink-level differences (realpath) are still not resolved.
- The first (raw) parse still runs without flags; flags only affect the second, preprocessed C/C++ parse (which is where macro-hidden calls come from) — consistent with the existing `cbm_extract_file` design.
- Incremental **sequential** path (changed files ≤ 50) still lacks cross-LSP — it builds no `CBMFileResult` cache to feed the pass.

---

## 9. Self-review (code-review skill, 3 finder agents) — findings & resolutions

A multi-agent review of the implementation diff (correctness + concurrency + cleanup angles)
confirmed the concurrency was safe (`cbm_ht_get` is read-only; concurrent per-file lookups in
`extract_worker` are race-free) but surfaced real issues, now resolved:

- **[HIGH] WS7 incremental cross-LSP was changed-set-only → REVERTED.** `cbm_pipeline_pass_lsp_cross`
  builds its project-wide def list from the per-file result cache, which on the incremental path
  holds only the *changed* files. So it could not resolve calls into *unchanged* files (and the
  fuzzy fallback could mis-bind to a same-named changed symbol), diverging from a full index.
  Correct incremental cross-file resolution needs the full-project def set (WS1B/WS1C) — deferred.
  The call was removed and the rationale documented inline in `run_extract_resolve`.
- **[HIGH] Incremental left `ctx.cc_index` NULL → FIXED.** `cbm_pipeline_run_incremental` now
  builds/sets/frees `cc_index`, so a changed C/C++ file is re-extracted with the same
  `compile_commands` flags as a full index (per-file extraction is order-independent, so no
  divergence here).
- **[MED] 5th `cbm_extract_file` site (`pass_usages.c`) still passed `NULL,NULL` → FIXED.**
  (The two `pass_k8s.c` sites are KUSTOMIZE/K8S YAML, never in `compile_commands.json`, so
  correctly left unwired.)
- **[LOW] 4× copy-pasted lookup+cast → FIXED.** Added `cbm_cc_index_defines()` /
  `cbm_cc_index_includes()` accessors; all 5 call sites now call them (single source of the
  NULL-guard + const-cast).
- **[LOW] `cbm_extract_flags` unchecked `calloc` → FIXED.** NULL-checks both arrays, frees and
  returns NULL on OOM.
- **[NIT] `CC_FLAG_SKIP` reused as hash-table sizing multiplier → FIXED.** Replaced with a
  dedicated `CC_INDEX_HT_LOAD` constant.
- **[LOW] Triple-duplicated parsed-array teardown → FIXED.** Factored into `cc_free_parsed()`.
- **[LOW] `f->standard` (`-std`) parsed but unused.** Unchanged — already tracked above as a
  documented limitation (threading it churns ~30 callers for marginal value).

Net effect: WS2 (compile_commands) is now wired consistently across full **and** incremental
extraction with a single accessor; WS7 (incremental cross-file LSP) is explicitly deferred with
an inline rationale rather than shipped in a divergent form.

### 9.1 Follow-up fix (second "fix all" pass)
- **compile_commands path canonicalization → FIXED.** Added a lexical `normalize_path()` (collapses
  `.`/`..`/`//`) applied to each entry's resolved path before the repo-prefix match, so non-CMake
  generators (`"file":"./foo.c"`, directories containing `..`) now match the clean repo-relative
  lookup keys. Verified end-to-end. (realpath/symlink resolution still out of scope.)
- **`-std` propagation — still documented, not threaded.** Reason recorded precisely: `cbm_extract_file`
  has 28 call sites (mostly tests); adding a std param is a wide signature churn for marginal value
  (simplecpp's c11/c++17 default is adequate for macro expansion). Revisit if a std-gated macro bug appears.
- **WS1A (parallelize cross-LSP) — still deferred, reason now precise:** `cbm_slab_install` redirects
  tree-sitter's allocator via `ts_set_allocator` to a *thread-local* slab (`slab_alloc.c:202`), and the
  cross-LSP re-parse uses `ts_parser_new` (`py_lsp.c`, `go_lsp.c`). Parallelizing needs per-worker slab
  lifecycle management + a TSan run — a real feature, not a blind edit.
