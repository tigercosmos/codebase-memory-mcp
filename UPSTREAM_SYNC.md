# Upstream Sync

How to keep this fork (`tigercosmos/cpp-codebase-memory-mcp`, a C++23 port) in
sync with upstream **[`DeusData/codebase-memory-mcp`](https://github.com/DeusData/codebase-memory-mcp)** (C11).

## Last synced

| | |
|---|---|
| **Upstream commit** | `b133a204d224f644ade0c07adaac3f5a04ead90b` (release v0.7.0, 2026-05-30) |
| **Synced on** | 2026-05-30 |

Update both fields whenever you pull new upstream work (see the log at the bottom).

## Why you can't just `git merge upstream/main`

Git's merge-base for the two histories is an old commit
(`a0e809d`), so a raw merge tries to replay **hundreds** of commits we already
have (via earlier syncs) and conflicts massively with this fork's structural
changes. **Do not run `git merge upstream/main` or a blind `git cherry-pick`.**
Sync by porting the *genuinely new* upstream commits by hand (procedure below).

### Structural divergences from upstream (the conflict landscape)

These are intentional and permanent; expect every upstream commit that touches
them to need translation, not a clean apply:

- **Language: C11 → C++23.** Every first-party `.c` was renamed to `.cpp`
  (e.g. upstream `src/pipeline/pass_parallel.c` ⇄ our `…/pass_parallel.cpp`).
  An upstream diff to a `.c` file applies to our same-named `.cpp`.
- **Build system: `Makefile.cbm` (deleted) → CMake** (`CMakeLists.txt`,
  `build/c/…`). Upstream Makefile/CI changes have no direct counterpart; the CI
  wrapper scripts (`scripts/{build,test,lint,security}.sh`) drive CMake.
- **C++ idioms** in some TUs (e.g. `std::array`/IIFE tables in `language.cpp`,
  `lang_specs.cpp`; `cbm_type_args()` instead of compound literals in
  `ts_lsp.cpp`/`cs_lsp.cpp`/`generated/*.cpp`) — context around upstream edits
  there will differ.
- **Vendored libraries stay C.** sqlite3, mongoose, yyjson, lz4, zstd, tre,
  ts_runtime, and the tree-sitter `grammar_*.c` are compiled as C (they don't
  port to C++ without forking generated/upstream code). The `grammar_*.c` and
  `ts_runtime.c` are zero-logic `#include` shims.
- **`-Werror` is GCC-only** for first-party C++ (clang's extra warnings aren't
  promoted to errors). Lint uses clang-format **20.1.8** + cppcheck in **C++**
  mode (with C-idiom checks suppressed). Run `scripts/lint.sh` before pushing.
- **Fork rebrand.** All repo URLs point to `tigercosmos/cpp-codebase-memory-mcp`
  (README credits upstream). Skip upstream's release/versioning/`pkg/*` and
  registry (`server.json`) changes unless intentionally re-adopting them.

## Sync procedure

```bash
git fetch upstream main

# 1. List upstream commits NEWER than our last-synced marker that we DON'T
#    already have (compare by message — SHAs diverge across the fork).
MARKER=b133a204d224f644ade0c07adaac3f5a04ead90b   # <- the "Last synced" SHA above
git log --format='%s' "$MARKER"..upstream/main | while IFS= read -r m; do
  git log --format='%s' main | grep -qxF "$m" || echo "NEW: $m"
done
```

For each genuinely-new commit:

1. **Triage.** Skip upstream-only machinery: release/version bumps, `RELEASE_NOTES_*`,
   `pkg/*` publishing, `Makefile.cbm`, CI workflow internals that don't apply.
   Keep substantive **bug fixes**, **perf**, **features**, **test** changes.
2. **Inspect** the diff: `git show <sha>`.
3. **Apply by hand** to our tree — map `.c` → our `.cpp`; adapt context for the
   C++ idioms / `-Werror` / vendored-stays-C rules above. (Cherry-pick usually
   fails on the rename + context drift; manual is more reliable.)
4. **Verify**: `scripts/test.sh` (expect `3629 passed, 1 failed` under ASan —
   the lone failure is the environment-sensitive `test_incremental` RSS check)
   and `scripts/lint.sh` (or at least `clang-format-20.1.8 --dry-run --Werror`
   over the changed files).
5. **Commit** crediting the upstream SHA(s) in the message
   (`merge(upstream): … from DeusData/codebase-memory-mcp@<sha>`).
6. **Update** the "Last synced" SHA/date above and add a row to the log.

## Sync log

| Date | Upstream → | Ported | Skipped |
|------|-----------|--------|---------|
| 2026-05-30 | `b133a20` (v0.7.0) | `82d5c84` parallel slab/parser use-after-free fix; `297003a` + `130b707` (test parts) sanitizer-aware LSP benches | `b133a20` pkg version bump, `130b707` release-notes doc, `d5211f6` notes-file removal |
| (earlier) | `9a9488f`-era | full upstream history up to the "pad source read buffers / gate LSP benchmarks" era, brought in before the C++ migration | — |
