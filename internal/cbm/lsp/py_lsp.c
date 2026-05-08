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

void py_lsp_process_file(PyLSPContext* ctx, TSNode root) {
    /* Phase 2 stub: no-op. Phase 4+ walks the AST and binds/resolves. */
    (void)ctx;
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
