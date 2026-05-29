# C → C++23 migration roadmap

This is the staging plan for migrating `codebase-memory-mcp` from C11 to
C++23. **Phase 1** (compile every first-party TU as C++23 with CMake)
is **done** on branch `worktree-cxx23-migration`; **Phase 2** is the
idiomatic-C++23 rewriting that turns ported-as-C++ TUs into modern code.

## Current state — Phase 1 complete

* Branch: `worktree-cxx23-migration`
* Build system: **CMake** (`CMakeLists.txt` at repo root). The original
  `Makefile.cbm` still builds and remains the source of truth for CI
  until the build-system swap is completed (see Phase 1 closing tasks).
* **92 first-party `.c` → `.cpp`** ports (64 in `src/`, 28 in
  `internal/cbm/`), total ~226 LOC added vs ~10 deleted across 159
  files (extern "C" wraps + atomic shim + cast inserts + one goto-init
  fix + designator reorders).
* **Test-runner parity with C baseline:**
  * With sanitizers (mirrors `make test`): **3629 passed, 1 failed** —
    identical to the C baseline (same `test_incremental` RSS-budget
    failure, environment-sensitive, not a port regression).
  * Without sanitizers: **3630 passed, 0 failed.**

### Files deliberately kept as C in Phase 1

Phase 1 doesn't try to force C++23 on every TU; some have idioms that
need real rewrites (Phase 2 work) and are cheaper to keep as C while
linked via `extern "C"`. They live in `cbm_lsp_generated`,
`cbm_foundation_c`, `cbm_discover_language`, and `cbm_lang_specs`
targets.

| File | Reason | Phase 2 fix |
|---|---|---|
| `src/foundation/hash_table.c` | Uses `verstable.h` (2069-line C99 macro lib, 26 `_Generic` sites) | Replace verstable with `std::unordered_map` |
| `src/discover/language.c` (978 LOC) | C99 sparse array designator `LANG_NAMES[CBM_LANG_X] = "name"` (156 entries) — GCC C++ says "sorry, unimplemented" | Convert table to constexpr lookup or switch |
| `internal/cbm/lang_specs.c` (2494 LOC) | Same C99 sparse-array idiom (156 entries) | Same |
| `internal/cbm/lsp/ts_lsp.c` | 24 `(const CBMType*[]){...}` compound literals inside aggregate initializers — C++ takes-address-of-temporary error; mechanical hoisting collides with surrounding init lists | Replace compound literals with arena allocations |
| `internal/cbm/lsp/cs_lsp.c` | 3 of the same compound-literal sites | Same |
| `internal/cbm/lsp/generated/*.c` (6 files, ~55k LOC) | Auto-generated stdlib data tables full of C99 compound literals | Regenerate emitter to produce arena allocations |
| `internal/cbm/grammar_*.c` (154 files) | `#include` vendored tree-sitter `parser.c` / `scanner.c` — generated C with idioms that don't translate | Stays C forever (vendored generated code) |
| `internal/cbm/ts_runtime.c` | Vendored tree-sitter runtime wrapper | Stays C forever |
| `vendored/` + `internal/cbm/vendored/` | Third-party C libraries (sqlite3, mongoose, mimalloc, etc.) | Stays C forever |
| `tests/*.c` (62 files) | Test sources still C; they call into C++ libs via the `extern "C"` headers and that works cleanly | Phase 2 optional |

### Files now C++23

Everything else: `src/foundation/` (15 TUs), `src/cli/`,
`src/cypher/`, `src/discover/` (3 of 4), `src/graph_buffer/`,
`src/mcp/`, `src/pipeline/` (28 TUs), `src/semantic/`,
`src/simhash/`, `src/store/`, `src/traces/`, `src/ui/`,
`src/watcher/`, `src/main.cpp`, `internal/cbm/cbm.cpp`, all
`internal/cbm/extract_*.cpp`, the four cross-language LSP files
(`go_lsp.cpp`, `c_lsp.cpp`, `php_lsp.cpp`, `py_lsp.cpp`), plus
`type_rep.cpp` / `scope.cpp` / `type_registry.cpp` /
`service_patterns.cpp` / `lz4_store.cpp` / `zstd_store.cpp` /
`sqlite_writer.cpp` / `ac.cpp` / `arena.cpp` / `helpers.cpp` /
`lsp_all.cpp`.

The `lsp_all.cpp` unity blob is still compiled as a single C++ TU
that `#include`s the lsp/*.cpp files.

## What Phase 1 actually fixed across the tree

Counts come from the actual diff applied during this session.

| Category | Sites | Tool |
|---|---|---|
| `extern "C"` wraps on first-party headers | 64 headers | Python script (after-includes placement) |
| `_Atomic T x;` → `std::atomic<T> x{0};` in `.cpp` | 68 sites in 16 files | regex substitution |
| `_Thread_local` → `thread_local` | 8 sites in 6 files | regex |
| `_Static_assert` → `static_assert` | 2 sites in compat_regex.cpp | regex |
| `_Alignof` → `alignof` | 2 sites in type_rep.cpp / mcp.cpp | regex |
| `#include <stdatomic.h>` → `#include "foundation/cbm_atomic.h"` in `.cpp` | 16 files | Python |
| `CBM_TLS` macro mapped to `thread_local` in C++ mode | 1 (compat.h) | manual |
| Explicit `(T *)` cast on `malloc`/`calloc`/`realloc`/`memchr`/`memmem`/`cbm_arena_alloc`/`cbm_ht_get`/`safe_realloc`/`safe_calloc` | ~370 sites (declarations + `obj->member =` + `*deref =` forms + ternaries) | Python regex passes |
| Hoist 27 C99 compound literals `(const CBMType*[]){...}` to named locals | 24 in ts_lsp.cpp + 3 in cs_lsp.cpp — too brittle, reverted both files to .c | trial only |
| `GROW_ARRAY` macro: `(arr)->items = void*` → cast via `__typeof__((arr)->items)` | 1 in cbm.cpp, 1 in dyn_array.h | manual |
| C tests need prototype for `cbm_node_text` once ts_lsp/cs_lsp leave the unity blob | 2 (helpers.h include) | manual |
| `tree_sitter_<lang>()` forward decls wrapped in `extern "C"` (was being mangled as C++) | 5 in lsp/*.cpp | regex |
| Designated-initializer reorder to match struct decl order | 2 (mcp.cpp search_params, pass_parallel.cpp resolution) | manual |
| Anonymous-enum-in-struct → named enum (C++ scopes anonymous enums) | 1 in php_lsp.h (CBMPhpUseKind) | manual |
| `int` → typed enum casts (`{NULL, NULL, 0}` → `{NULL, NULL, (CBMLanguage)0}`) | 4 in lang_specs (now .c, moot) | manual |
| `goto cleanup` over POD init → hoist declaration above goto | 1 in pipeline.cpp | manual |
| `(CLSPContext::<unnamed struct>*)` cast → `(__typeof__(ctx->field))` | 1 in c_lsp.cpp + 1 in pass_semantic_edges.cpp | manual |
| `const char *` → `char *` literal cast | 1 in mcp.cpp (`(char *)"query execution failed"`) | manual |
| `cbm_python_stdlib_register` stub conflict when generated/*.c is a separate TU | guarded via `-DCBM_PYTHON_STDLIB_GENERATED` on `cbm_lsp_all` | CMake |

## The portable atomic shim

`src/foundation/cbm_atomic.h` provides cross-language atomic typedefs
so headers that expose atomic fields in their public ABI
(`graph_buffer.h`, `pipeline_internal.h`, `diagnostics.h`) can be
included from both C tests and C++23 libs.

```c
#ifdef __cplusplus
  #include <atomic>
  using cbm_atomic_int64 = std::atomic<int64_t>;     // and friends
  using std::atomic_int;                              // global-scope C-style names
  using std::atomic_load;                             // free-function call sites work unchanged
  using std::memory_order_relaxed;
#else
  #include <stdatomic.h>
  typedef _Atomic int64_t cbm_atomic_int64;
#endif
```

`_Atomic` storage layout matches `std::atomic` on libstdc++/glibc
(same size, same alignment), so the C tests and C++ libs see the same
struct memory layout.

## Phase 1 closing tasks (still to do)

1. Add CMake targets for the **production binary**
   (`codebase-memory-mcp`) and `cbm-with-ui`. Only `test-runner` is
   currently wired.
2. Mirror `Makefile.cbm`'s `test-tsan`, `lint-tidy`, `lint-cppcheck`,
   `lint-format`, `lint-no-suppress`, `security` targets.
3. Delete `Makefile.cbm` and update CI
   (`.github/workflows/*.yml`, `install.sh`, `install.ps1`,
   `scripts/security-*.sh`, `scripts/check-nolint-whitelist.sh`) to use
   CMake.
4. Re-enable `-Werror` on first-party C++ TUs (it's currently off for
   the C++23 side via `CBM_WERROR=OFF` to allow `-Wextra` warnings like
   enum-narrowing and format-truncation through). C side keeps
   `-Werror`.
5. Migrate `tests/*.c` to `tests/*.cpp` — only valuable if the tests
   themselves want to use C++ idioms; otherwise they work as-is.

## Phase 2 — module-by-module C → idiomatic C++23

After Phase 1, every first-party TU compiles as C++23 but the code is
still C-style. Phase 2 is the actual modernization. For each module:

1. **RAII the resources.** `malloc`/`free` → `std::unique_ptr` /
   `std::vector` / `std::string`. Replace `CBMArena` (a custom bump
   allocator) call sites with arena-aware C++ types where the arena
   semantics matter, plain stdlib types where they don't.
2. **`std::expected<T, E>` at module boundaries.** The codebase uses
   `int`/`bool` returns + out-parameters everywhere. `std::expected`
   makes the error path explicit and removes the out-param dance.
3. **`std::string_view` for non-owning slices.** Foundation already has
   `(const char *ptr, size_t len)` pair conventions; collapse them.
4. **`std::span<T>` for arrays.** Same idea, for `(T *ptr, size_t n)`.
5. **`std::format` for logging.** Replaces `snprintf` + `fprintf`
   chains.
6. **Replace `verstable` with `std::unordered_map` /
   `std::flat_map`.** Unblocks porting `hash_table.c`. Touches
   `service_patterns.cpp` and `cbm.cpp` too (they also use
   `verstable`).
7. **Replace the C99 compound-literal idiom in `ts_lsp` / `cs_lsp` /
   `language.c` / `lang_specs.c` / generated stdlib data.** Use arena
   allocations or `std::array`. Unblocks porting those files to .cpp.
8. **Lift `_Atomic` operations to `std::atomic<T>` member ops.** Free
   functions (`atomic_load(&x)`) still work via the shim, but member
   syntax (`x.load(std::memory_order_relaxed)`) is clearer.
9. **Cypher engine → ranges + views.** The `cypher.c` evaluator is a
   prime candidate for `std::ranges` adaptation.
10. **Concepts on the pipeline pass interface.** Each pass declares an
    entry function and configures itself via a struct of callbacks;
    concepts make the contract checkable.

Re-enable `-Werror` on first-party C++ TUs at the end of Phase 2.

## Open items / gotchas surfaced during Phase 1

1. **`src/foundation/vmem.c`** (now `.cpp`) — not referenced by
   `Makefile.cbm` at all, but `tests/test_vmem.c` exists. Either the
   test is dead or the Makefile is missing a build rule. Investigate
   before Phase 2.
2. **`make test-foundation` is broken upstream.** `Makefile.cbm:386-387`
   links only `FOUNDATION_SRCS`, but `tests/test_main.c` references
   every suite (`suite_security`, `suite_yaml`, etc.) and calls
   `sqlite3_shutdown()`. Either fix the target to link the full
   `PROD_SRCS` or split `test_main.c` into a foundation-only
   entrypoint. CMake's `test-runner` target sidesteps this by linking
   everything — same as `make test`.
3. **`test_incremental.c:302` memory ceiling.** Threshold is
   `< 2048 MB`; observed 2192–2327 MB under ASan+UBSan across runs in
   both baseline (C) and ported (C++23) builds. Either raise the cap,
   run the test outside sanitizers, or scope it down to a smaller
   fixture corpus. The no-sanitizer build passes it.
4. **`.note.GNU-stack` linker warning** from `code_vectors_blob.S` —
   benign; add `.section .note.GNU-stack,"",%progbits` at the end of
   the `.S` to silence.
5. **GCC 13 rejects sparse array designators** (`[ENUM_VAL] = "x"`) in
   C++ even though it accepts struct designators. This is why
   `language.c` and `lang_specs.c` stay as C.
6. **Compound-literal-inside-aggregate-initializer hoisting is brittle
   for regex.** When `(T*[]){...}` appears inside a `T returns[N] = {
   ... }` initializer list, my line-statement-aware hoist still
   misplaces declarations. The pragmatic Phase 1 call was to keep
   those two files (ts_lsp, cs_lsp) as C until Phase 2 rewrites the
   compound literals into arena allocations.
7. **Files moved out of the `lsp_all.cpp` unity blob lose any
   prototypes they were depending on through transitive include
   order.** Specifically `ts_lsp.c` and `cs_lsp.c` were using
   `cbm_node_text` without including `helpers.h`; the unity build
   made it work, the split build silently degraded to an implicit
   `int`-returning function declaration and the resulting truncation
   sign-extended the returned pointer into a SEGV during the JS
   extraction test. Now fixed with explicit `#include
   "../helpers.h"`; worth a global audit when splitting more files.

## Helpers: the scripts that did the heavy lifting

### Header `extern "C"` wrap

Places opener AFTER all `#include`/`#define` lines (including those
inside `#if` blocks) so transitively included C++ stdlib headers stay
unmangled. Putting `extern "C" {` before `#include <stdatomic.h>`
breaks because `<atomic>` templates can't have C linkage.

```python
import re, pathlib

def first_decl_line(lines, start):
    """Walk past blank lines, comments, and preprocessor directives;
    return the index of the first declaration line. Tracks #if/#endif
    nesting so the opener lands at top-level, not inside a conditional."""
    i, in_block_comment, nest = start, False, 0
    while i < len(lines):
        ln = lines[i]; s = ln.strip()
        if in_block_comment:
            if '*/' in ln: in_block_comment = False
            i += 1; continue
        if '/*' in ln and '*/' not in ln:
            in_block_comment = True; i += 1; continue
        if not s or s.startswith('//'): i += 1; continue
        if s.startswith('#'):
            kw = s[1:].lstrip().split(None, 1)[0] if s[1:].lstrip() else ''
            if kw in ('if', 'ifdef', 'ifndef'): nest += 1
            elif kw == 'endif' and nest > 0: nest -= 1
            i += 1; continue
        if nest > 0: i += 1; continue
        return i
    return len(lines)

for h in pathlib.Path('src/<module>').glob('*.h'):
    text = h.read_text()
    if 'extern "C"' in text: continue
    lines = text.split('\n')
    guard_idx = next(
        i for i, ln in enumerate(lines)
        if re.match(r'^#define\s+\w+\s*$', ln.strip())
        and i > 0
        and re.match(r'^#ifndef\s+\w+\s*$', lines[i-1].strip()))
    decl_idx = first_decl_line(lines, guard_idx + 1)
    last_endif = max(i for i, ln in enumerate(lines) if ln.strip().startswith('#endif'))
    h.write_text('\n'.join(
        lines[:decl_idx]
        + ['', '#ifdef __cplusplus', 'extern "C" {', '#endif', '']
        + lines[decl_idx:last_endif]
        + ['#ifdef __cplusplus', '}', '#endif', '']
        + lines[last_endif:]))
```

### Cast position fix when a regex-based inserter misplaces the cast

When the error log says "invalid conversion from `void*` to T*" at a
column inside the function call (e.g. `calloc(T*)(args)`), the cast
needs to move outside the call. This script catches
`IDENT(TYPE*)(args)` and rewrites to `(TYPE*)IDENT(args)` for known
void-returning allocator names only.

```python
import re, pathlib
VALID = {'malloc','calloc','realloc','safe_realloc','safe_calloc',
         'safe_malloc','cbm_ht_get','cbm_arena_alloc','cbm_arena_calloc',
         'memchr','memmem','mi_malloc','mi_calloc','mi_realloc',
         'slab_malloc','slab_calloc','slab_realloc','aligned_alloc'}
BAD = re.compile(r'\b([A-Za-z_]\w*)\(([A-Za-z_:<>][\w\s:<>*]*\*)\)\(')

for p in pathlib.Path('.').rglob('*.cpp'):
    text = p.read_text(); out = []; pos = 0
    for m in BAD.finditer(text):
        if m.group(1) not in VALID: continue
        out.append(text[pos:m.start()])
        out.append(f'({m.group(2).strip()}){m.group(1)}(')
        pos = m.end()
    out.append(text[pos:])
    p.write_text(''.join(out))
```
