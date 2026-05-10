/*
 * pass_lsp_cross.c — Cross-file LSP type-aware call resolution pass.
 *
 * See pass_lsp_cross.h for the high-level contract. This file is the
 * pipeline glue that converts the existing per-file extraction state
 * (CBMDefinition / CBMImport / IMPORTS-edge gbuf state) into the input
 * shape each language LSP's cbm_run_X_lsp_cross expects, then merges
 * the resulting CBMResolvedCall entries back into per-file results.
 *
 * The pass is a no-op for any file whose CBMFileResult is missing or
 * whose language has no cross-file LSP entry registered (e.g. Rust /
 * Java today). Per-LSP emit functions dedup against entries already in
 * resolved_calls, so this pass is also idempotent — safe to invoke
 * multiple times if the pipeline gains a re-run path later.
 */
#include "pipeline/pass_lsp_cross.h"
#include "pipeline/pipeline_internal.h"
#include "lsp/go_lsp.h"
#include "lsp/c_lsp.h"
#include "lsp/py_lsp.h"
#include "lsp/ts_lsp.h"
#include "lsp/php_lsp.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/constants.h"
#include "foundation/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ─────────────────────────────────────────────────── */

enum {
    PXC_MAX_FILE_BYTES_FACTOR = 100, /* same cap pass_calls.c uses for source size */
    PXC_ITOA_BUF = 16,
};

/* Format an int into a thread-local rotating buffer for log key=value emission.
 * Mirrors the itoa_log helper in pass_calls.c — kept local so passes don't
 * grow a foundation-wide formatting API just for log output. */
static const char *itoa_buf(int val) {
    static _Thread_local char bufs[PXC_ITOA_BUF][PXC_ITOA_BUF];
    static _Thread_local int slot = 0;
    char *out = bufs[slot];
    slot = (slot + 1) & (PXC_ITOA_BUF - 1);
    snprintf(out, PXC_ITOA_BUF, "%d", val);
    return out;
}

/* ── Local helpers ─────────────────────────────────────────────── */

/* Slurp a file into a malloc'd, NUL-terminated buffer. Mirrors the
 * read_file helper in pass_calls.c / pass_parallel.c (kept local so the
 * pipeline doesn't grow a public read-file API just for this pass). */
static char *pxc_read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (size <= 0 ||
        size > (long)PXC_MAX_FILE_BYTES_FACTOR * (long)CBM_SZ_1K * (long)CBM_SZ_1K) {
        (void)fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    (void)fclose(f);
    if (nread > (size_t)size) nread = (size_t)size;
    buf[nread] = '\0';
    *out_len = (int)nread;
    return buf;
}

/* Map a CBMDefinition.label to a CBMLSPDef.label. Per-language LSP
 * registrars only care about Class/Interface/Trait/Enum/Type/Protocol/
 * Function/Method — variables, modules, decorators, etc. are skipped. */
static const char *pxc_map_label(const char *label) {
    if (!label) return NULL;
    if (strcmp(label, "Class") == 0 ||
        strcmp(label, "Interface") == 0 ||
        strcmp(label, "Trait") == 0 ||
        strcmp(label, "Enum") == 0 ||
        strcmp(label, "Type") == 0 ||
        strcmp(label, "Protocol") == 0 ||
        strcmp(label, "Function") == 0 ||
        strcmp(label, "Method") == 0) {
        return label;
    }
    return NULL;
}

/* Build the embedded_types "|"-separated string from base_classes[].
 * Returns NULL when there are no bases. Allocated in the supplied arena. */
static const char *pxc_join_pipe(CBMArena *arena, const char *const *items) {
    if (!items || !items[0]) return NULL;
    int count = 0;
    size_t total = 0;
    for (int i = 0; items[i]; i++) {
        count++;
        total += strlen(items[i]);
    }
    if (count == 0) return NULL;
    /* count - 1 separators + NUL. */
    size_t bufsz = total + (size_t)(count - 1) + 1;
    char *buf = (char *)cbm_arena_alloc(arena, bufsz);
    if (!buf) return NULL;
    char *p = buf;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(items[i]);
        memcpy(p, items[i], n);
        p += n;
        if (i + 1 < count) *p++ = '|';
    }
    *p = '\0';
    return buf;
}

/* Convert one CBMDefinition into a CBMLSPDef. Returns 0 on success, -1
 * to skip (unsupported label or missing required field). dst gets borrowed
 * pointers into src and into `arena` for synthesised composites. */
static int pxc_build_lsp_def(CBMArena *arena, const CBMDefinition *src,
                              const char *module_qn, CBMLSPDef *dst) {
    const char *label = pxc_map_label(src->label);
    if (!label || !src->qualified_name || !src->name) return -1;
    memset(dst, 0, sizeof(*dst));
    dst->qualified_name = src->qualified_name;
    dst->short_name = src->name;
    dst->label = label;
    dst->receiver_type = src->parent_class;
    dst->def_module_qn = module_qn;
    dst->is_interface = (strcmp(label, "Interface") == 0 ||
                          strcmp(label, "Protocol") == 0);
    /* Single return-type string. The per-language registrars split on '|'
     * for multi-return languages (Go); single-return languages just see one
     * piece, which is what's already stored. */
    dst->return_types = src->return_type;
    dst->embedded_types = pxc_join_pipe(arena, src->base_classes);
    return 0;
}

/* Collect a project-wide CBMLSPDef[] from all cached results. Returns a
 * malloc'd array (caller frees) of length *out_count. String fields are
 * borrowed from cache[i]->arena and from def_modules[i] (also borrowed). */
static CBMLSPDef *pxc_collect_all_defs(CBMFileResult **cache,
                                       const cbm_file_info_t *files, int file_count,
                                       const char *project_name,
                                       char **def_modules, int *out_count) {
    int total = 0;
    for (int i = 0; i < file_count; i++) {
        if (cache[i]) total += cache[i]->defs.count;
    }
    if (total == 0) {
        *out_count = 0;
        return NULL;
    }
    CBMLSPDef *defs = (CBMLSPDef *)calloc((size_t)total, sizeof(CBMLSPDef));
    if (!defs) {
        *out_count = 0;
        return NULL;
    }
    int idx = 0;
    for (int fi = 0; fi < file_count; fi++) {
        if (!cache[fi]) continue;
        if (!def_modules[fi]) {
            def_modules[fi] = cbm_pipeline_fqn_module(project_name, files[fi].rel_path);
        }
        for (int di = 0; di < cache[fi]->defs.count; di++) {
            if (pxc_build_lsp_def(&cache[fi]->arena,
                                   &cache[fi]->defs.items[di],
                                   def_modules[fi],
                                   &defs[idx]) == 0) {
                idx++;
            }
        }
    }
    *out_count = idx;
    return defs;
}

/* Build per-file import map (local_name -> resolved module QN) from gbuf
 * IMPORTS edges. Mirrors build_import_map() in pass_parallel.c. Returns 0
 * with *out_count = 0 when the file has no IMPORTS edges. Caller frees keys
 * with pxc_free_import_map. */
static int pxc_build_import_map(const cbm_gbuf_t *gbuf, const char *project_name,
                                 const char *rel_path, const char ***out_keys,
                                 const char ***out_vals, int *out_count) {
    *out_keys = NULL;
    *out_vals = NULL;
    *out_count = 0;

    char *file_qn = cbm_pipeline_fqn_compute(project_name, rel_path, "__file__");
    if (!file_qn) return 0;
    const cbm_gbuf_node_t *file_node = cbm_gbuf_find_by_qn(gbuf, file_qn);
    free(file_qn);
    if (!file_node) return 0;

    const cbm_gbuf_edge_t **edges = NULL;
    int edge_count = 0;
    int rc = cbm_gbuf_find_edges_by_source_type(gbuf, file_node->id, "IMPORTS",
                                                  &edges, &edge_count);
    if (rc != 0 || edge_count == 0) return 0;

    const char **keys = (const char **)calloc((size_t)edge_count, sizeof(const char *));
    const char **vals = (const char **)calloc((size_t)edge_count, sizeof(const char *));
    if (!keys || !vals) {
        free(keys);
        free(vals);
        return 0;
    }
    int count = 0;
    for (int i = 0; i < edge_count; i++) {
        const cbm_gbuf_edge_t *e = edges[i];
        const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, e->target_id);
        if (!target || !e->properties_json) continue;
        const char *start = strstr(e->properties_json, "\"local_name\":\"");
        if (!start) continue;
        start += strlen("\"local_name\":\"");
        const char *end = strchr(start, '"');
        if (!end || end <= start) continue;
        size_t n = (size_t)(end - start);
        char *local = (char *)malloc(n + 1);
        if (!local) continue;
        memcpy(local, start, n);
        local[n] = '\0';
        keys[count] = local;
        vals[count] = target->qualified_name; /* borrowed from gbuf */
        count++;
    }
    *out_keys = keys;
    *out_vals = vals;
    *out_count = count;
    return 0;
}

static void pxc_free_import_map(const char **keys, const char **vals, int count) {
    if (keys) {
        for (int i = 0; i < count; i++) free((void *)keys[i]);
        free((void *)keys);
    }
    free((void *)vals); /* vals strings borrowed from gbuf — don't free elements */
}

/* Detect TS dialect flags from a relative path. */
static void pxc_ts_modes(CBMLanguage lang, const char *rel_path,
                          bool *out_js, bool *out_jsx, bool *out_dts) {
    *out_js = (lang == CBM_LANG_JAVASCRIPT);
    *out_jsx = (lang == CBM_LANG_TSX);
    *out_dts = false;
    if (!rel_path) return;
    size_t rl = strlen(rel_path);
    if (lang == CBM_LANG_JAVASCRIPT && rl >= 4 &&
        strcmp(rel_path + rl - 4, ".jsx") == 0) {
        *out_jsx = true;
    }
    if (lang == CBM_LANG_TYPESCRIPT && rl >= 5 &&
        strcmp(rel_path + rl - 5, ".d.ts") == 0) {
        *out_dts = true;
    }
}

/* Returns true when this language has a cross-file LSP wired up. */
static bool pxc_has_cross_lsp(CBMLanguage lang) {
    switch (lang) {
    case CBM_LANG_GO:
    case CBM_LANG_C:
    case CBM_LANG_CPP:
    case CBM_LANG_CUDA:
    case CBM_LANG_PYTHON:
    case CBM_LANG_JAVASCRIPT:
    case CBM_LANG_TYPESCRIPT:
    case CBM_LANG_TSX:
    case CBM_LANG_PHP:
        return true;
    default:
        return false;
    }
}

/* True when (caller_qn, callee_qn) is already present in arr. Mirrors the
 * intra-LSP dedup in py_emit_resolved_call / ts_emit_resolved_call so that
 * cross-file entries duplicating per-file output get dropped at append. */
static bool pxc_already_resolved(const CBMResolvedCallArray *arr,
                                  const char *caller_qn, const char *callee_qn) {
    if (!arr || !caller_qn || !callee_qn) return false;
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (rc->caller_qn && rc->callee_qn &&
            strcmp(rc->caller_qn, caller_qn) == 0 &&
            strcmp(rc->callee_qn, callee_qn) == 0) {
            return true;
        }
    }
    return false;
}

/* Append cross-file results from `src_out` (allocated in a scratch arena
 * about to be destroyed) into `dst_calls` (lives in cache_entry->arena),
 * copying every string field into dst_arena. Skips entries whose
 * (caller_qn, callee_qn) is already in dst_calls — avoids inflating the
 * array with cross-file duplicates of per-file LSP output. */
static void pxc_append_results(CBMArena *dst_arena, CBMResolvedCallArray *dst_calls,
                                const CBMResolvedCallArray *src_out) {
    if (!dst_calls || !src_out) return;
    for (int j = 0; j < src_out->count; j++) {
        const CBMResolvedCall *src = &src_out->items[j];
        if (!src->caller_qn || !src->callee_qn) continue;
        if (pxc_already_resolved(dst_calls, src->caller_qn, src->callee_qn)) continue;
        CBMResolvedCall dst;
        memset(&dst, 0, sizeof(dst));
        dst.caller_qn = cbm_arena_strdup(dst_arena, src->caller_qn);
        dst.callee_qn = cbm_arena_strdup(dst_arena, src->callee_qn);
        dst.strategy = src->strategy ? cbm_arena_strdup(dst_arena, src->strategy) : NULL;
        dst.confidence = src->confidence;
        dst.reason = src->reason ? cbm_arena_strdup(dst_arena, src->reason) : NULL;
        cbm_resolvedcall_push(dst_calls, dst_arena, dst);
    }
}

/* Run cross-file LSP for a single file inside a scratch arena that gets
 * freed when the call returns. The LSP would otherwise allocate a fresh
 * type registry + stdlib + all project defs into the supplied arena, and
 * that adds up to O(N×project_size) memory if we used cache[i]->arena
 * directly across N files (test_incremental.c saw 3.5 GB peak on a
 * 1100-file repo before this fix). Output gets copied into the file's own
 * arena and merged into result->resolved_calls. */
static void pxc_run_one(CBMLanguage lang, CBMFileResult *r, const char *source,
                         int source_len, const char *module_qn,
                         CBMLSPDef *defs, int def_count,
                         const char **imp_names, const char **imp_qns, int imp_count) {
    TSTree *tree = r->cached_tree; /* may be NULL — LSP re-parses then */

    CBMArena scratch;
    cbm_arena_init(&scratch);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));

    switch (lang) {
    case CBM_LANG_GO:
        cbm_run_go_lsp_cross(&scratch, source, source_len, module_qn,
                              defs, def_count, imp_names, imp_qns, imp_count, tree,
                              &out);
        break;
    case CBM_LANG_C:
    case CBM_LANG_CPP:
    case CBM_LANG_CUDA: {
        bool cpp_mode = (lang != CBM_LANG_C);
        /* C/C++ cross LSP takes include_paths/include_ns_qns instead of
         * imports — the existing pipeline doesn't carry C-style include
         * resolution as a separate map, so pass NULL/0 and let the LSP
         * fall back to its own #include scan. */
        cbm_run_c_lsp_cross(&scratch, source, source_len, module_qn, cpp_mode,
                             defs, def_count, NULL, NULL, 0, tree,
                             &out);
        break;
    }
    case CBM_LANG_PYTHON:
        cbm_run_py_lsp_cross(&scratch, source, source_len, module_qn,
                              defs, def_count, imp_names, imp_qns, imp_count, tree,
                              &out);
        break;
    case CBM_LANG_PHP:
        cbm_run_php_lsp_cross(&scratch, source, source_len, module_qn,
                               defs, def_count, imp_names, imp_qns, imp_count, tree,
                               &out);
        break;
    default:
        break;
    }

    pxc_append_results(&r->arena, &r->resolved_calls, &out);
    cbm_arena_destroy(&scratch);
}

/* Variant of pxc_run_one for TS/JS/JSX/TSX with explicit dialect flags.
 * Same scratch-arena lifecycle as pxc_run_one. */
static void pxc_run_one_ts(CBMFileResult *r, const char *source, int source_len,
                            const char *module_qn,
                            CBMLSPDef *defs, int def_count,
                            const char **imp_names, const char **imp_qns, int imp_count,
                            bool js_mode, bool jsx_mode, bool dts_mode) {
    CBMArena scratch;
    cbm_arena_init(&scratch);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));

    cbm_run_ts_lsp_cross(&scratch, source, source_len, module_qn,
                          js_mode, jsx_mode, dts_mode,
                          defs, def_count, imp_names, imp_qns, imp_count,
                          r->cached_tree, &out);

    pxc_append_results(&r->arena, &r->resolved_calls, &out);
    cbm_arena_destroy(&scratch);
}

int cbm_pipeline_pass_lsp_cross(cbm_pipeline_ctx_t *ctx,
                                const cbm_file_info_t *files,
                                int file_count,
                                CBMFileResult **cache) {
    if (!ctx || !files || file_count <= 0 || !cache) return 0;

    cbm_log_info("pass.start", "pass", "lsp_cross", "files",
                 itoa_buf(file_count));

    /* Per-file module QN cache so we don't recompute it once per def + once
     * per call. cbm_pipeline_fqn_module mallocs; freed at end. */
    char **def_modules = (char **)calloc((size_t)file_count, sizeof(char *));
    if (!def_modules) {
        cbm_log_error("pass.err", "pass", "lsp_cross", "phase", "alloc");
        return 0;
    }

    int def_count = 0;
    CBMLSPDef *all_defs = pxc_collect_all_defs(cache, files, file_count,
                                                ctx->project_name, def_modules,
                                                &def_count);

    int processed = 0;
    int skipped_no_lsp = 0;
    int skipped_no_source = 0;
    int per_lang_calls = 0;

    for (int i = 0; i < file_count; i++) {
        if (!cache[i]) continue;
        CBMLanguage lang = files[i].language;
        if (!pxc_has_cross_lsp(lang)) {
            skipped_no_lsp++;
            continue;
        }

        int source_len = 0;
        char *source = pxc_read_file(files[i].path, &source_len);
        if (!source || source_len <= 0) {
            free(source);
            skipped_no_source++;
            continue;
        }

        if (!def_modules[i]) {
            def_modules[i] = cbm_pipeline_fqn_module(ctx->project_name, files[i].rel_path);
        }

        const char **imp_keys = NULL;
        const char **imp_vals = NULL;
        int imp_count = 0;
        pxc_build_import_map(ctx->gbuf, ctx->project_name, files[i].rel_path,
                              &imp_keys, &imp_vals, &imp_count);

        if (lang == CBM_LANG_JAVASCRIPT || lang == CBM_LANG_TYPESCRIPT ||
            lang == CBM_LANG_TSX) {
            bool js, jsx, dts;
            pxc_ts_modes(lang, files[i].rel_path, &js, &jsx, &dts);
            pxc_run_one_ts(cache[i], source, source_len, def_modules[i],
                            all_defs, def_count, imp_keys, imp_vals, imp_count,
                            js, jsx, dts);
        } else {
            pxc_run_one(lang, cache[i], source, source_len, def_modules[i],
                         all_defs, def_count, imp_keys, imp_vals, imp_count);
        }
        per_lang_calls++;
        processed++;

        pxc_free_import_map(imp_keys, imp_vals, imp_count);
        free(source);
    }

    free(all_defs);
    for (int i = 0; i < file_count; i++) free(def_modules[i]);
    free(def_modules);

    cbm_log_info("pass.done", "pass", "lsp_cross",
                 "files_processed", itoa_buf(processed),
                 "files_skipped_no_lsp", itoa_buf(skipped_no_lsp),
                 "files_skipped_no_source", itoa_buf(skipped_no_source),
                 "defs_total", itoa_buf(def_count),
                 "lsp_calls", itoa_buf(per_lang_calls));
    return 0;
}
