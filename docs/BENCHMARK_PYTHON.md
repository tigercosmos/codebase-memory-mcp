# Python LSP — Phase 11 benchmark methodology

This document describes how to measure the Python LSP type resolver
(`internal/cbm/lsp/py_lsp.c`, Phases 0–10) against real Python code.
Numbers from a specific corpus run can be appended in a "Results"
section as new measurements land.

## Targets (locked in PYTHON_LSP_PLAN.md)

| Metric | Target |
|---|---|
| Per-file LSP overhead (absolute) | < 10 ms for a 500-line file |
| `resolved_calls.count / calls.count` (application code) | ≥ 40% |
| `resolved_calls.count / calls.count` (with stdlib resolution) | ≥ 70% |
| Manual spot-check false-positive rate (50 sampled resolutions) | < 5% |

## Accepted failure classes (declared ceiling)

The following resolution failures are considered correct UNKNOWN, not
benchmark regressions. Surface area outside this list is the measurable
gap to investigate:

1. Metaclass `__call__` — class instantiation through a custom metaclass.
2. `__getattr__` / `__getattribute__` — dynamic attribute resolution at
   runtime; py_lsp bails to UNKNOWN as documented in Phase 6.
3. `setattr(obj, name, value)` where `name` is not a literal string.
4. `importlib.import_module(<dynamic_arg>)` — import path computed at
   runtime.
5. Runtime monkey-patching of methods or classes after definition.
6. `eval` / `exec` of source strings.
7. Descriptors (`__get__`, `__set__`, `__delete__`) beyond `@property`,
   `@classmethod`, `@staticmethod`.
8. `__call__` on instances of classes whose `__call__` is dynamically
   bound.
9. `__init_subclass__` hooks that mutate the subclass after creation.
10. Dynamically generated classes via `type(name, bases, dict)`.

Anything outside this list that produces UNKNOWN is a known gap
candidate for Phase 11.x extensions or v1.1.

## How to measure

### Fast: in-process integration test

`tests/test_py_lsp_bench.c` runs `cbm_extract_file` on a representative
Python source string and reports `calls.count` /
`resolved_calls.count` plus extraction time. Run with:

```bash
scripts/test.sh
# Look for the "py_lsp_bench" suite output
```

### Full: against a Python-heavy repository

```bash
# 1. Build a fresh production binary from this worktree.
scripts/build.sh

# 2. Run benchmark-index against a Python-heavy repo. Suggested
#    candidates (Python file counts as of 2026-05-09):
#      ~/project_dir/datadice/falkemedia (232 .py files)
#      ~/project_dir/datadice/api-hub    (188 .py files)
mkdir -p /tmp/py-bench-results
scripts/benchmark-index.sh \
  ./build/c/codebase-memory-mcp \
  python \
  ~/project_dir/datadice/falkemedia \
  /tmp/py-bench-results

# 3. Compute the resolution ratio. The benchmark dumps node/edge counts
#    plus per-file timings; cross-reference with calls / resolved_calls
#    via the MCP graph queries:
codebase-memory-mcp query \
  "MATCH ()-[r:CALLS]->() WHERE r.strategy STARTS WITH 'lsp_' \
   RETURN count(r) AS lsp_resolved"

codebase-memory-mcp query \
  "MATCH ()-[r:CALLS]->() RETURN count(r) AS total_resolved"
```

### Manual spot-check

Sample 50 resolved-call edges (random offset in the CALLS edge set)
and verify by hand:

- Open the source file at `caller_qn`'s line.
- Locate the call expression that produced the edge.
- Confirm the `callee_qn` matches what a human reader would identify.

A false positive count of 0–2 out of 50 (0–4%) is within the < 5%
target. 3+ false positives on the sample → triage and document
contributing patterns; usually they cluster on a specific dynamic
construct (e.g. `getattr`-style dispatch).

## Results

(Appended as benchmark runs land. None recorded yet at the time of
Phase 11 wrap-up; the methodology and integration test are in place
for when a fresh production binary is built.)
