# PR title

fix(lsp): guard NULL pointer derefs in c_resolve_pending_template_calls (per-file C LSP SIGSEGV)

---

# PR description

## Problem

Indexing certain templated C++ files reliably crashes with **SIGSEGV (exit 139)**. The crash is in the per-file C LSP pass (`cbm_run_c_lsp` → `c_lsp_process_file` → `c_resolve_calls_in_node` → `c_resolve_pending_template_calls`), reached from `cbm_extract_file`. Because the C LSP runs during per-file extraction, it reproduces in every index mode (`fast`, `moderate`, and `full`) — the `mode: "moderate"` workaround from the cross-file `lsp_cross` crashes does not help here, since this is a different (per-file) code path. The backtrace is shallow (`c_resolve_calls_in_node` recurses only ~3 levels), so this is a NULL/dangling pointer dereference, not stack exhaustion.

```
#0  c_resolve_pending_template_calls ()
#1  c_resolve_calls_in_node ()
#2  c_resolve_calls_in_node ()
#3  c_resolve_calls_in_node ()
#4  c_process_body_child ()
#6  c_lsp_process_file ()
#7  cbm_run_c_lsp ()
#8  cbm_extract_file ()
```

## Root cause

`c_resolve_pending_template_calls` (`internal/cbm/lsp/c_lsp.c`) dereferences several fields that can be NULL, while its top-of-function guard only covers `callee`, `callee->type_param_names`, and `call_arg_types`.

The fields it reads inside the loop — the per-pending-call `func_qn` / `type_param` / `method_name`, and the type-param name `formal->data.type_param.name` — are all produced by `cbm_arena_strdup`, which returns NULL when the underlying arena allocation fails. Under heavy memory pressure (the reporting repo is a large monorepo with a 32 GB arena budget) any of these can be stored as NULL and then dereferenced here.

The most likely crash site is `strcmp(tpn[j], tp)` with a NULL `tp` (from `pending_template_calls[i].type_param`), though the diff alone does not prove which deref fired. The other unguarded reads are `strcmp(..., func_qn)` with a NULL `func_qn`, `c_lookup_member(ctx, concrete_qn, method)` with a NULL `method`, and `strcmp(tpn[j], formal->data.type_param.name)`.

There is also a latent bound-check-order bug: `while (tpn[tpn_count] && tpn_count < 8)` reads `tpn[tpn_count]` before the `tpn_count < 8` test short-circuits, so with 8+ type params it reads one element past the intended bound.

## Reproduction

Deterministic, exits 139 every time, on a 423-line gtest C++ file with nested templated classes deriving from a templated base, 11 `TEST_F` macros, and a trailing test whose body is a single helper call (`RunValidationTest(this, {...}, {...}, {...}, false, -10.0)`) with nested brace-initializer lists:

```bash
mkdir -p /tmp/repro && cp <offending-file>.cpp /tmp/repro/
codebase-memory-mcp cli index_repository '{"repo_path":"/tmp/repro","mode":"fast"}'
# -> Segmentation fault (core dumped); exit 139
```

Line bisection isolates the trigger to the closing brace of that final test function (`head -396` indexes fine, `head -397` crashes), consistent with `c_resolve_pending_template_calls` walking `ctx->pending_template_calls` after accumulating the file's earlier templated-class state. The exact trigger depends on accumulated template/registry state and arena pressure, so a minimal standalone synthetic file does not reproduce the original crash; the fix is therefore defensive — NULL-guarding each field the loop reads — and the added test exercises the same trigger pattern as a no-crash smoke test rather than forcing the specific NULL branch.

## Fix

All production-code changes are in `c_resolve_pending_template_calls` (`internal/cbm/lsp/c_lsp.c`), plus a regression test in `tests/test_c_lsp.c`. They only add NULL guards and reorder a bound check, with no behavioral change for valid input.

- Reorder the type-param count loop to `while (tpn_count < 8 && tpn[tpn_count]) tpn_count++;` so the `< 8` bound is checked before the array read.
- Guard `formal->data.type_param.name` before `strcmp` when matching call-arg types to formal type params.
- Guard the pending-call `func_qn` before `strcmp(fqn, callee->qualified_name)`.
- Skip pending calls whose `type_param` or `method_name` is NULL (`if (!tp || !method) continue;`), which covers both the `strcmp(tpn[j], tp)` deref and the `c_lookup_member(..., method)` lookup.

These guards cover the NULL derefs in this function; `callee->qualified_name` is left as-is, matching the existing pre-guard behavior.

## Testing

Validated locally on this branch:

- Builds clean (`make -f Makefile.cbm cbm`).
- Full suite under AddressSanitizer + UndefinedBehaviorSanitizer (`scripts/test.sh`, clean build): all C-LSP tests pass with zero ASan/UBSan reports. (The one unrelated failure is `test_incremental.c` `incr_full_index`, the RSS-budget assertion the test itself documents as calibrated for CI's sparse checkout vs a full local checkout.)
- The reproducer pattern, indexed end-to-end via the CLI in `fast` mode, now exits 0 instead of 139.
- Adds regression test `clsp_nocrash_template_derived_brace_init_call` in `tests/test_c_lsp.c`, modeling the trigger: a CRTP templated class deriving from a templated base (whose `static_cast<Derived*>(this)->handle(...)` populates `pending_template_calls`) plus a trailing helper call with nested brace-initializer lists. It is a no-crash smoke test over the resolution path, not a forced reproduction of the specific NULL-field branch.

Related: #344, #340, #312, #336, #355
Fixes #<ISSUE>  <!-- file the per-file C LSP SIGSEGV issue and reference its number here -->
