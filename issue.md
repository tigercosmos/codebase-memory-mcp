# SIGSEGV in `c_resolve_pending_template_calls` (c_lsp.c) during per-file C/C++ extraction

## Summary

Indexing a large C++ monorepo reliably crashes with **SIGSEGV (exit 139)**. The
crash is in the **per-file C LSP pass** (`cbm_run_c_lsp` -> `c_lsp_process_file`
-> `c_resolve_calls_in_node` -> `c_resolve_pending_template_calls`), reached
from `cbm_extract_file`.

Because the C LSP runs during per-file extraction, the crash reproduces in
**every** index mode -- `fast`, `moderate`, *and* `full`. So the `mode:
"moderate"` workaround noted in #344 / #340 (which skips the cross-file
`lsp_cross` pass) does **not** avoid this one. This is the per-file C LSP, a
different code path from the cross-file `lsp_cross` SIGSEGV in those issues.

The crash is isolated to a **single source file** out of ~2,735; every other
file indexes cleanly.

## Environment

- **Binary**: `codebase-memory-mcp 0.6.1`, prebuilt release, x86-64, *not
  stripped* (symbols available).
- **OS**: Linux 6.17.0-20-generic (Ubuntu), x86-64.
- **RAM**: 64 GB total (`mem.init budget_mb=32062 total_ram_mb=64124`).
- **Invocation**: reproduces both through the MCP server (Claude Code) and via
  the direct CLI (`codebase-memory-mcp cli index_repository ...`). Through the
  MCP server the symptom is the client seeing `MCP error -32000: Connection
  closed` as the server process dies.
- **Repo under test**: a large C++ autonomous-driving monorepo, ~2,735 files
  reaching the extractor under `src/`.

## Backtrace

Captured with `gdb --batch` against the unstripped release binary
(`set debuginfod enabled off`, bare `run`):

```
Program received signal SIGSEGV, Segmentation fault.
0x0000555556289ec9 in c_resolve_pending_template_calls ()
#0  0x0000555556289ec9 in c_resolve_pending_template_calls ()
#1  0x0000555556290b88 in c_resolve_calls_in_node ()
#2  0x000055555628dfe2 in c_resolve_calls_in_node ()
#3  0x000055555628dfe2 in c_resolve_calls_in_node ()
#4  0x0000555556292f17 in c_process_body_child ()
#5  0x000055555629274f in c_process_body_child ()
#6  0x00005555562947be in c_lsp_process_file ()
#7  0x000055555629ea37 in cbm_run_c_lsp ()
#8  0x0000555555856060 in cbm_extract_file ()
#11 0x00005555557e312b in try_incremental_or_delete_db ()
#13 0x00005555557d62b5 in handle_index_repository ()
#14 0x00005555557a4254 in main ()
```

Source: `internal/cbm/lsp/c_lsp.c`.

Note the backtrace is shallow -- only ~14 frames, with `c_resolve_calls_in_node`
recursing just 3 levels (frames #1-#3; #2 and #3 share the same return address
`0x...dfe2`, a self-recursive call site). This is **not** stack exhaustion; it
points to a **null / dangling pointer dereference** inside
`c_resolve_pending_template_calls`.

## Reproduction

Deterministic, exits 139 every time:

```bash
mkdir -p /tmp/repro && cp <the-offending-file>.cpp /tmp/repro/
codebase-memory-mcp cli index_repository '{"repo_path":"/tmp/repro","mode":"fast"}'
# -> Segmentation fault (core dumped); exit 139
```

### The offending file

A 423-line, ~16.5 KB gtest C++ file (proprietary, not included here). Its
relevant structural features:

- Nested **templated** classes deriving from a templated base, e.g.
  `template <typename Derived, typename InputType> class SubscriberBase :
  public ::apex::executor::apex_node_base { ... }`.
- 11 `TEST_F`-style macros.
- A trailing test function whose body is a single call to a helper
  (`RunValidationTest(this, {...}, {...}, {...}, false, -10.0)`) with several
  **nested brace-initializer lists**.

### Minimization (line bisection)

Binary-searching `head -N` of the file:

- `head -396` indexes fine.
- `head -397` crashes.

Line 397 is the **closing brace** of that final test function. So the crash
fires when the parser finishes resolving the templated call construct after
accumulating the file's earlier templated-class state -- consistent with
`c_resolve_pending_template_calls` walking `ctx->pending_template_calls`. I was
not able to reduce it to a small standalone synthetic file in a couple of quick
attempts (a bare templated-derived class + nested-brace call, and a
forward-declared template call resolved later, both index cleanly), so the
trigger depends on more specific accumulated template/registry state.

### Scope / isolation

Indexing each `src/` subsystem separately (CLI, `moderate`): 8 of 9 succeed
(including `planning` at 26,509 nodes). Only `perception` crashes. Within
`perception`, only `tracking/` crashes. Within `tracking/`, a per-file probe
(each file copied alone into a temp dir, indexed) isolates the crash to exactly
**one** file: `.../tracking/test/tracking_component_test.cpp`.

## Suspected root cause

`c_resolve_pending_template_calls` (`internal/cbm/lsp/c_lsp.c:291`) performs
several `strcmp` / pointer operations on fields that are not guaranteed
non-NULL. Candidates, in order of suspicion:

1. **`c_lsp.c:337` -- `strcmp(tpn[j], tp)` with `tp` unchecked** (prime
   suspect). `tp` comes straight from
   `ctx->pending_template_calls[i].type_param` (line 332) and is never
   null-checked before the `strcmp`:

   ```c
   const char* tp = ctx->pending_template_calls[i].type_param;   // 332
   const char* method = ctx->pending_template_calls[i].method_name; // 333
   ...
   for (int j = 0; j < tpn_count; j++) {                          // 336
       if (strcmp(tpn[j], tp) != 0 || !param_map[j]) continue;    // 337  <-- tp may be NULL
   ```

2. **`c_lsp.c:341` -- `c_lookup_member(ctx, concrete_qn, method)`** passes
   `method` (line 333), which may be NULL, into a lookup that likely `strcmp`s
   it.

3. **`c_lsp.c:330` -- `strcmp(ctx->pending_template_calls[i].func_qn, callee->qualified_name)`**
   -- `func_qn` not null-checked.

4. **`c_lsp.c:316` -- `strcmp(tpn[j], formal->data.type_param.name)`** --
   `formal->data.type_param.name` not null-checked.

5. **`c_lsp.c:299` -- `while (tpn[tpn_count] && tpn_count < 8) tpn_count++;`**
   -- separate latent bug: the bound check is evaluated *after* the array read,
   so with 8+ type params this reads `tpn[8]` (one past the intended bound)
   before the `tpn_count < 8` short-circuits. Reorder to
   `while (tpn_count < 8 && tpn[tpn_count])`.

The guard at the top only covers `callee`, `callee->type_param_names`, and
`call_arg_types`:

```c
if (!callee || !callee->type_param_names || !call_arg_types) return;
```

It does not cover the per-pending-call fields (`type_param`, `method_name`,
`func_qn`) that get dereferenced in the resolution loop.

## Suggested fix

Add null guards in the pending-call loop, and fix the bound-check order:

```c
while (tpn_count < 8 && tpn[tpn_count]) tpn_count++;            // 299: reorder
...
for (int i = 0; i < ctx->pending_tc_count; i++) {
    const char* fqn = ctx->pending_template_calls[i].func_qn;
    if (!fqn || strcmp(fqn, callee->qualified_name) != 0) continue;   // 330
    const char* tp = ctx->pending_template_calls[i].type_param;
    const char* method = ctx->pending_template_calls[i].method_name;
    if (!tp || !method) continue;                                     // new guard
    for (int j = 0; j < tpn_count; j++) {
        if (!tpn[j] || strcmp(tpn[j], tp) != 0 || !param_map[j]) continue; // 337
        ...
    }
}
```

Building with `-fsanitize=address` and re-running the repro file against
`internal/cbm/lsp/c_lsp.c` should pin the exact dereference to one of the lines
above.

## Workaround (no code change)

Exclude the single offending file via `.cbmignore` and re-index; the rest of
the tree indexes cleanly. Note that no `mode` flag avoids the crash, since the
per-file C LSP runs in `fast`, `moderate`, and `full`.

## Related issues

- **#344**, **#340** -- SIGSEGV in the *cross-file* `lsp_cross` pass
  (scale-dependent). Related family, different pass; this report is the
  *per-file* C LSP (`cbm_run_c_lsp` from `cbm_extract_file`), with a precise
  backtrace into `c_resolve_pending_template_calls`.
- **#312** ("crash on simple C++ snippet"), **#336** ("Segmentation fault"),
  **#355** ("segfault on a specific .h") -- likely the same C-extractor /
  C-LSP memory-safety family.
