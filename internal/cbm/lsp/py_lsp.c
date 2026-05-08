/*
 * py_lsp.c — Python type-aware call resolution.
 *
 * Phase 2 scaffold: PyLSPContext, single-file entry point, cross-file +
 * batch entry points (no-op shells). Subsequent phases fill in:
 *   Phase 3 — imports
 *   Phase 4 — scope binding
 *   Phase 5 — expression typing
 *   Phase 6 — attribute resolution + method dispatch
 *   Phase 7 — decorators
 *   Phase 8 — class hierarchy + generics
 *   Phase 9 — cross-file resolution + batch
 *   Phase 10 — cbm_python_stdlib_register
 */
#include "py_lsp.h"
#include "../cbm.h"
#include "../helpers.h"
#include "tree_sitter/api.h"
#include <stdlib.h>
#include <string.h>

void py_lsp_init(PyLSPContext* ctx, CBMArena* arena, const char* source, int source_len,
    const CBMTypeRegistry* registry, const char* module_qn, CBMResolvedCallArray* out) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(PyLSPContext));
    ctx->arena = arena;
    ctx->source = source;
    ctx->source_len = source_len;
    ctx->registry = registry;
    ctx->module_qn = module_qn;
    ctx->resolved_calls = out;
    ctx->current_scope = cbm_scope_push(arena, NULL);
    const char* dbg = getenv("CBM_LSP_DEBUG");
    ctx->debug = dbg && dbg[0] && dbg[0] != '0';
}

void py_lsp_add_import(PyLSPContext* ctx, const char* local_name, const char* module_qn) {
    if (!ctx || !local_name || !module_qn) return;

    int new_count = ctx->import_count + 1;
    const char** names = (const char**)cbm_arena_alloc(ctx->arena,
        (size_t)(new_count + 1) * sizeof(const char*));
    const char** qns = (const char**)cbm_arena_alloc(ctx->arena,
        (size_t)(new_count + 1) * sizeof(const char*));
    if (!names || !qns) return;

    for (int i = 0; i < ctx->import_count; i++) {
        names[i] = ctx->import_local_names[i];
        qns[i] = ctx->import_module_qns[i];
    }
    names[ctx->import_count] = cbm_arena_strdup(ctx->arena, local_name);
    qns[ctx->import_count] = cbm_arena_strdup(ctx->arena, module_qn);
    names[new_count] = NULL;
    qns[new_count] = NULL;

    ctx->import_local_names = names;
    ctx->import_module_qns = qns;
    ctx->import_count = new_count;
}

/* Determine whether this import is an `import X` style binding (binds the
 * module itself) or a `from X import Y` style binding (binds Y, an
 * attribute of module X). Heuristic: if module_qn ends in `.<local_name>`
 * and has at least two components, it's a from-import; otherwise it's a
 * straight module import. Matches the shape produced by extract_imports.c.
 */
static bool import_is_from_style(const char* local_name, const char* module_qn) {
    if (!local_name || !module_qn) return false;
    size_t local_len = strlen(local_name);
    size_t mod_len = strlen(module_qn);
    if (mod_len <= local_len) return false;
    if (module_qn[mod_len - local_len - 1] != '.') return false;
    if (strcmp(module_qn + mod_len - local_len, local_name) != 0) return false;
    return true;
}

void py_lsp_bind_imports(PyLSPContext* ctx) {
    if (!ctx || !ctx->current_scope) return;
    for (int i = 0; i < ctx->import_count; i++) {
        const char* local = ctx->import_local_names[i];
        const char* qn = ctx->import_module_qns[i];
        if (!local || !qn) continue;

        // Wildcard imports are recorded for traceability but cannot bind a
        // concrete name — skip the scope insertion. Phase 9 cross-file
        // logic will use the import map directly to find re-exports.
        if (strcmp(local, "*") == 0) continue;

        const CBMType* t;
        if (import_is_from_style(local, qn)) {
            // `from X import Y` — bind Y to NAMED(X.Y). Phase 6 attribute
            // resolution checks the registry to upgrade to MODULE / class
            // / function as appropriate.
            t = cbm_type_named(ctx->arena, qn);
        } else {
            // `import X` / `import X as Y` — bind to MODULE(X).
            t = cbm_type_module(ctx->arena, qn);
        }
        cbm_scope_bind(ctx->current_scope, local, t);
    }
}

const CBMType* py_lsp_lookup_in_scope(const PyLSPContext* ctx, const char* name) {
    if (!ctx) return cbm_type_unknown();
    return cbm_scope_lookup(ctx->current_scope, name);
}

void py_lsp_process_file(PyLSPContext* ctx, TSNode root) {
    if (!ctx) return;
    py_lsp_bind_imports(ctx);
    /* Phase 4+ walks the AST and binds locals + resolves calls. */
    (void)root;
}

/* ── cbm_run_py_lsp: single-file entry point ──────────────────── */

void cbm_run_py_lsp(CBMArena* arena, CBMFileResult* result,
    const char* source, int source_len, TSNode root) {
    if (!arena || !result) return;

    CBMTypeRegistry reg;
    cbm_registry_init(&reg, arena);

    // Phase 10 will register typeshed-generated stdlib here.
    cbm_python_stdlib_register(&reg, arena);

    const char* module_qn = result->module_qn;

    // Phase 4 will register the file's own definitions into reg here.

    PyLSPContext ctx;
    py_lsp_init(&ctx, arena, source, source_len, &reg, module_qn, &result->resolved_calls);

    // Wire imports from the unified extractor.
    for (int i = 0; i < result->imports.count; i++) {
        CBMImport* imp = &result->imports.items[i];
        if (imp->local_name && imp->module_path) {
            py_lsp_add_import(&ctx, imp->local_name, imp->module_path);
        }
    }

    py_lsp_process_file(&ctx, root);
}

/* ── Cross-file + batch — Phase 9 fills these in ──────────────── */

void cbm_run_py_lsp_cross(
    CBMArena* arena,
    const char* source, int source_len,
    const char* module_qn,
    CBMLSPDef* defs, int def_count,
    const char** import_names, const char** import_qns, int import_count,
    TSTree* cached_tree,
    CBMResolvedCallArray* out) {
    (void)arena;
    (void)source;
    (void)source_len;
    (void)module_qn;
    (void)defs;
    (void)def_count;
    (void)import_names;
    (void)import_qns;
    (void)import_count;
    (void)cached_tree;
    (void)out;
}

void cbm_batch_py_lsp_cross(
    CBMArena* arena,
    CBMBatchPyLSPFile* files, int file_count,
    CBMResolvedCallArray* out) {
    (void)arena;
    (void)files;
    (void)file_count;
    (void)out;
}

/* ── Stdlib stub — Phase 10 replaces with auto-generated body ─── */

#ifndef CBM_PYTHON_STDLIB_GENERATED
void cbm_python_stdlib_register(CBMTypeRegistry* reg, CBMArena* arena) {
    (void)reg;
    (void)arena;
}
#endif
