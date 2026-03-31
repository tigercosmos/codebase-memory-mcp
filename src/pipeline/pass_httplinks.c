
/*
 * pass_httplinks.c — HTTP route discovery and cross-service linking.
 *
 * Port of Go internal/httplink package. Discovers HTTP route registrations
 * (Express, Flask, Gin, Spring, Ktor, Laravel, Actix, ASP.NET) and HTTP
 * call sites (fetch, http.Get, requests.post, etc.). Creates:
 *   - Route nodes with method + path
 *   - HANDLES edges: handler function → Route
 *   - HTTP_CALLS edges: caller → Route (cross-service calls)
 *   - ASYNC_CALLS edges: caller → Route (async dispatch)
 *   - CALLS edges with via=route_registration (registrar → handler)
 *
 * Operates on graph buffer (pre-flush): reads Function/Method/Module nodes,
 * parses decorator properties via yyjson, reads source from disk, and writes
 * Route nodes + edges back to the graph buffer.
 *
 * Depends on: pass_definitions, pass_calls (for cross-file prefix resolution)
 */
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/httplink.h"
#include "pipeline/worker_pool.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/platform.h"
#include "foundation/log.h"
#include "foundation/compat.h"
#include "foundation/compat_regex.h"

#include "yyjson/yyjson.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* ── Constants ────────────────────────────────────────────────── */
#define DOTTED_FRAG_BUF 260      /* buffer for slash-to-dot path conversion */
#define MIN_PATH_CONFIDENCE 0.25 /* minimum score to create HTTP_CALLS edge */
#define MODULE_WEIGHT 0.85       /* confidence weight for Module-sourced calls */

/* ── Format int to string for logging ──────────────────────────── */

static const char *itoa_hl(int val) {
    static CBM_TLS char bufs[4][32];
    static CBM_TLS int idx = 0;
    int i = idx;
    idx = (idx + 1) & 3;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

/* ── Source cache helpers ──────────────────────────────────────── */

/* Read source lines from disk (used for route discovery which processes
 * few files filtered by language extension). */
static char *read_source_lines(const cbm_pipeline_ctx_t *ctx, const char *rel_path, int start_line,
                               int end_line) {
    return cbm_read_source_lines_disk(ctx->repo_path, rel_path, start_line, end_line);
}

/* Read full source file, using cache if available. Returns malloc'd copy (caller must free). */
/* Read full source from disk (used for module route discovery — few files,
 * filtered by .php/.js extension). */
static char *read_full_source(const cbm_pipeline_ctx_t *ctx, const char *rel_path) {
    char path_buf[2048];
    snprintf(path_buf, sizeof(path_buf), "%s/%s", ctx->repo_path, rel_path);
    FILE *f = fopen(path_buf, "rb");
    if (!f) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)10 * 1024 * 1024) {
        (void)fclose(f);
        return NULL;
    }
    char *source = malloc((size_t)sz + 1);
    if (!source) {
        (void)fclose(f);
        return NULL;
    }
    size_t nread = fread(source, 1, (size_t)sz, f);
    (void)fclose(f);
    source[nread] = '\0';
    return source;
}

/* ── JSON helpers ──────────────────────────────────────────────── */

/* Extract the "decorators" array from a properties_json string.
 * Returns a NULL-terminated array of strings. Caller must free array and strings. */
static char **extract_decorators(const char *json, int *out_count) {
    *out_count = 0;
    if (!json) {
        return NULL;
    }

    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return NULL;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *decs = yyjson_obj_get(root, "decorators");
    if (!decs || !yyjson_is_arr(decs)) {
        yyjson_doc_free(doc);
        return NULL;
    }

    size_t cnt = yyjson_arr_size(decs);
    if (cnt == 0) {
        yyjson_doc_free(doc);
        return NULL;
    }

    char **out = calloc(cnt + 1, sizeof(char *));
    int idx = 0;
    yyjson_val *item;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(decs, &iter);
    while ((item = yyjson_arr_iter_next(&iter))) {
        if (yyjson_is_str(item)) {
            char *s = strdup(yyjson_get_str(item));
            /* Collapse newlines to spaces so regex matches multiline decorators.
             * POSIX regex [[:space:]] may not match \n on all platforms. */
            if (s) {
                for (char *p = s; *p; p++) {
                    if (*p == '\n' || *p == '\r') {
                        *p = ' ';
                    }
                }
            }
            out[idx++] = s;
        }
    }
    out[idx] = NULL;
    *out_count = idx;

    yyjson_doc_free(doc);
    if (idx > 0) {
        return out;
    }
    free(out);
    return NULL;
}

/* Check if a JSON properties string has is_test=true. */
static bool is_test_from_json(const char *json) {
    if (!json) {
        return false;
    }
    /* Fast path: substring search before full parse */
    if (!strstr(json, "\"is_test\"")) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *v = yyjson_obj_get(root, "is_test");
    bool result = v && yyjson_is_bool(v) && yyjson_get_bool(v);
    yyjson_doc_free(doc);
    return result;
}

/* Check if node is from a test file (file path heuristic + is_test property). */
static bool is_test_node(const cbm_gbuf_node_t *n) {
    if (is_test_from_json(n->properties_json)) {
        return true;
    }
    if (!n->file_path) {
        return false;
    }
    return cbm_is_test_node_fp(n->file_path, false);
}

/* Update properties_json to set is_entry_point=true.
 * Returns a newly allocated JSON string. Caller must free(). */
static char *set_entry_point(const char *json) {
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *mroot;

    if (root && yyjson_is_obj(root)) {
        mroot = yyjson_val_mut_copy(mdoc, root);
    } else {
        mroot = yyjson_mut_obj(mdoc);
    }
    yyjson_mut_doc_set_root(mdoc, mroot);

    yyjson_mut_obj_remove_key(mroot, "is_entry_point");
    yyjson_mut_obj_add_bool(mdoc, mroot, "is_entry_point", true);

    char *result = yyjson_mut_write(mdoc, 0, NULL);
    yyjson_mut_doc_free(mdoc);
    if (doc) {
        yyjson_doc_free(doc);
    }
    return result;
}

static void free_decorators(char **decs) {
    if (!decs) {
        return;
    }
    for (int i = 0; decs[i]; i++) {
        free(decs[i]);
    }
    free(decs);
}

/* ── Suffix helpers ────────────────────────────────────────────── */

static bool has_suffix(const char *s, const char *suffix) {
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (xl > sl) {
        return false;
    }
    return strcmp(s + sl - xl, suffix) == 0;
}

static bool is_jsts_file(const char *path) {
    return has_suffix(path, ".js") || has_suffix(path, ".ts") || has_suffix(path, ".mjs") ||
           has_suffix(path, ".mts") || has_suffix(path, ".tsx");
}

/* ── Route discovery ───────────────────────────────────────────── */

/* Max routes per pass */
#define MAX_ROUTES 4096
#define MAX_CALL_SITES 4096

/* Discover routes from a single Function/Method node. */
static int discover_node_routes(const cbm_gbuf_node_t *n, const cbm_pipeline_ctx_t *ctx,
                                cbm_route_handler_t *out, int max_out) {
    int total = 0;

    /* 1. Decorator-based routes (Python, Java, Rust, ASP.NET) */
    int ndec = 0;
    char **decs = extract_decorators(n->properties_json, &ndec);
    if (decs && ndec > 0) {
        int nr = cbm_extract_python_routes(n->name, n->qualified_name, (const char **)decs, ndec,
                                           out + total, max_out - total);
        total += nr;

        nr = cbm_extract_java_routes(n->name, n->qualified_name, (const char **)decs, ndec,
                                     out + total, max_out - total);
        total += nr;

        /* Rust Actix and C# ASP.NET also use decorator patterns —
         * these are handled by java_routes for similar decorator syntax
         * but we can add specific extractors here if needed */
    }
    free_decorators(decs);

    /* 2. Source-based routes — scoped by file extension to avoid
     * cross-framework false positives (e.g. Ktor regex matching PHP Cache::get) */
    if (n->file_path && n->start_line > 0 && n->end_line > 0 && total < max_out) {
        char *source = read_source_lines(ctx, n->file_path, n->start_line, n->end_line);
        if (source) {
            const char *fp = n->file_path;
            int nr;

            if (has_suffix(fp, ".go")) {
                nr = cbm_extract_go_routes(n->name, n->qualified_name, source, out + total,
                                           max_out - total);
                total += nr;
            }
            if (is_jsts_file(fp)) {
                nr = cbm_extract_express_routes(n->name, n->qualified_name, source, out + total,
                                                max_out - total);
                total += nr;
            }
            if (has_suffix(fp, ".php")) {
                nr = cbm_extract_laravel_routes(n->name, n->qualified_name, source, out + total,
                                                max_out - total);
                total += nr;
            }
            if (has_suffix(fp, ".kt") || has_suffix(fp, ".kts")) {
                nr = cbm_extract_ktor_routes(n->name, n->qualified_name, source, out + total,
                                             max_out - total);
                total += nr;
            }

            free(source);
        }
    }

    return total;
}

/* Discover module-level routes (PHP Laravel, JS/TS Express at top level). */
static int discover_module_routes(const cbm_gbuf_node_t *mod, const cbm_pipeline_ctx_t *ctx,
                                  cbm_route_handler_t *out, int max_out) {
    if (!mod->file_path) {
        return 0;
    }

    bool is_php = has_suffix(mod->file_path, ".php");
    bool is_js = is_jsts_file(mod->file_path);
    if (!is_php && !is_js) {
        return 0;
    }

    /* Read full file (from cache or disk) */
    char *source = read_full_source(ctx, mod->file_path);
    if (!source) {
        return 0;
    }

    int total = 0;
    if (is_php) {
        total += cbm_extract_laravel_routes(mod->name, mod->qualified_name, source, out + total,
                                            max_out - total);
    }
    if (is_js) {
        total += cbm_extract_express_routes(mod->name, mod->qualified_name, source, out + total,
                                            max_out - total);
    }
    free(source);
    return total;
}

/* ── Prefix resolution ─────────────────────────────────────────── */

/* Import map entry for prefix resolution (shared between FastAPI/Express). */
typedef struct {
    char var[128];
    char module[256];
} hl_import_entry_t;

/* Build Python import map from "from X import Y" statements. */
static int fastapi_build_import_map(const char *source, const cbm_regex_t *import_re,
                                    hl_import_entry_t *imports, int max_imports) {
    int count = 0;
    const char *p = source;
    cbm_regmatch_t pm[3];
    while (count < max_imports && cbm_regexec(import_re, p, 3, pm, 0) == 0) {
        int mlen = (pm[1].rm_eo - pm[1].rm_so);
        int vlen = (pm[2].rm_eo - pm[2].rm_so);
        if (mlen < 256 && vlen < 128) {
            snprintf(imports[count].module, 256, "%.*s", mlen, p + pm[1].rm_so);
            snprintf(imports[count].var, 128, "%.*s", vlen, p + pm[2].rm_so);
            count++;
        }
        p += pm[0].rm_eo;
    }
    return count;
}

/* Apply a resolved prefix to all routes matching a module path. */
static void apply_module_prefix(cbm_route_handler_t *routes, int route_count,
                                const char *module_path, const char *file_frag, char *prefix) {
    size_t pfx_len = strlen(prefix);
    while (pfx_len > 0 && prefix[pfx_len - 1] == '/') {
        prefix[--pfx_len] = '\0';
    }

    for (int r = 0; r < route_count; r++) {
        if (strncmp(routes[r].path, prefix, pfx_len) == 0) {
            continue;
        }
        if (strstr(routes[r].qualified_name, module_path) ||
            (routes[r].function_name[0] && strstr(routes[r].qualified_name, file_frag))) {
            char new_path[256];
            const char *old_path = routes[r].path;
            while (*old_path == '/') {
                old_path++;
            }
            snprintf(new_path, sizeof(new_path), "%s/%s", prefix, old_path);
            snprintf(routes[r].path, sizeof(routes[r].path), "%s", new_path);
        }
    }
}

/* Look up a variable name in the import map. */
static const char *hl_import_lookup(const hl_import_entry_t *imports, int count, const char *var) {
    for (int i = 0; i < count; i++) {
        if (strcmp(imports[i].var, var) == 0) {
            return imports[i].module;
        }
    }
    return NULL;
}

/* Process one Python module for include_router calls. */
static void fastapi_process_module(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *mod,
                                   const cbm_regex_t *include_re, const cbm_regex_t *import_re,
                                   cbm_route_handler_t *routes, int route_count) {
    char *source = read_full_source(ctx, mod->file_path);
    if (!source) {
        return;
    }

    hl_import_entry_t imports[64];
    memset(imports, 0, sizeof(imports));
    int import_count = fastapi_build_import_map(source, import_re, imports, 64);

    const char *p = source;
    cbm_regmatch_t pm[3];
    while (cbm_regexec(include_re, p, 3, pm, 0) == 0) {
        char var_name[128] = {0};
        char prefix[256] = {0};
        int vlen = (pm[1].rm_eo - pm[1].rm_so);
        int plen = (pm[2].rm_eo - pm[2].rm_so);
        if (vlen < 128) {
            snprintf(var_name, 128, "%.*s", vlen, p + pm[1].rm_so);
        }
        if (plen < 256) {
            snprintf(prefix, 256, "%.*s", plen, p + pm[2].rm_so);
        }
        p += pm[0].rm_eo;

        /* Resolve var → module path */
        const char *module_path = hl_import_lookup(imports, import_count, var_name);
        if (!module_path) {
            continue;
        }

        /* Convert dotted module path to file fragment */
        char file_frag[256];
        snprintf(file_frag, sizeof(file_frag), "%s", module_path);
        for (char *c = file_frag; *c; c++) {
            if (*c == '.') {
                *c = '/';
            }
        }

        apply_module_prefix(routes, route_count, module_path, file_frag, prefix);
    }

    free(source);
}

/* Resolve FastAPI include_router prefixes.
 * Scans Python Module nodes for: app.include_router(var, prefix="/prefix")
 * and from ... import var. Prepends prefix to routes from matching modules. */
static void resolve_fastapi_prefixes(cbm_pipeline_ctx_t *ctx, cbm_route_handler_t *routes,
                                     int route_count) {
    const cbm_gbuf_node_t **modules = NULL;
    int mod_count = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "Module", &modules, &mod_count) != 0) {
        return;
    }

    cbm_regex_t include_re;
    cbm_regex_t import_re;
    if (cbm_regcomp(
            &include_re,
            "\\.include_router\\(([[:alnum:]_]+)[[:space:]]*,[[:space:]]*prefix[[:space:]]*=[[:"
            "space:]]*[\"']([^\"']+)[\"']",
            CBM_REG_EXTENDED) != 0) {
        return;
    }
    if (cbm_regcomp(&import_re,
                    "from[[:space:]]+([[:alnum:]_.]+)[[:space:]]+import[[:space:]]+([[:alnum:]_]+)",
                    CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&include_re);
        return;
    }

    for (int m = 0; m < mod_count; m++) {
        const cbm_gbuf_node_t *mod = modules[m];
        if (!mod->file_path || !has_suffix(mod->file_path, ".py")) {
            continue;
        }
        fastapi_process_module(ctx, mod, &include_re, &import_re, routes, route_count);
    }

    cbm_regfree(&include_re);
    cbm_regfree(&import_re);
}

/* Build JS/TS import map from require() and ES import statements. */
static int express_build_import_map(const char *source, const cbm_regex_t *require_re,
                                    const cbm_regex_t *esimport_re, hl_import_entry_t *imports,
                                    int max_imports) {
    int count = 0;
    cbm_regmatch_t pm[4];

    const char *p = source;
    while (count < max_imports && cbm_regexec(require_re, p, 4, pm, 0) == 0) {
        int vlen = (pm[2].rm_eo - pm[2].rm_so);
        int mlen = (pm[3].rm_eo - pm[3].rm_so);
        if (vlen < 128 && mlen < 256) {
            snprintf(imports[count].var, 128, "%.*s", vlen, p + pm[2].rm_so);
            snprintf(imports[count].module, 256, "%.*s", mlen, p + pm[3].rm_so);
            count++;
        }
        p += pm[0].rm_eo;
    }
    p = source;
    while (count < max_imports && cbm_regexec(esimport_re, p, 3, pm, 0) == 0) {
        int vlen = (pm[1].rm_eo - pm[1].rm_so);
        int mlen = (pm[2].rm_eo - pm[2].rm_so);
        if (vlen < 128 && mlen < 256) {
            snprintf(imports[count].var, 128, "%.*s", vlen, p + pm[1].rm_so);
            snprintf(imports[count].module, 256, "%.*s", mlen, p + pm[2].rm_so);
            count++;
        }
        p += pm[0].rm_eo;
    }
    return count;
}

/* Apply Express .use() prefix for one matched var to routes. */
static void express_apply_use_prefix(cbm_route_handler_t *routes, int route_count,
                                     const char *module_path, char *prefix) {
    /* Strip leading ./ and ../ from relative import */
    const char *file_frag = module_path;
    if (strncmp(file_frag, "./", 2) == 0) {
        file_frag += 2;
    }
    if (strncmp(file_frag, "../", 3) == 0) {
        file_frag += 3;
    }

    /* Strip trailing slash from prefix */
    size_t pfx_len = strlen(prefix);
    while (pfx_len > 0 && prefix[pfx_len - 1] == '/') {
        prefix[--pfx_len] = '\0';
    }

    /* Convert slash-based path to dots for QN matching */
    char dotted_frag[DOTTED_FRAG_BUF];
    snprintf(dotted_frag, sizeof(dotted_frag), "%s", file_frag);
    for (char *c = dotted_frag; *c; c++) {
        if (*c == '/') {
            *c = '.';
        }
    }

    for (int r = 0; r < route_count; r++) {
        if (strncmp(routes[r].path, prefix, pfx_len) == 0) {
            continue;
        }
        if (strstr(routes[r].qualified_name, dotted_frag) ||
            strstr(routes[r].qualified_name, file_frag)) {
            char new_path[256];
            const char *old_path = routes[r].path;
            while (*old_path == '/') {
                old_path++;
            }
            snprintf(new_path, sizeof(new_path), "%s/%s", prefix, old_path);
            snprintf(routes[r].path, sizeof(routes[r].path), "%s", new_path);
        }
    }
}

/* Process one JS/TS module for Express .use() prefix resolution. */
static void express_process_module(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *mod,
                                   const cbm_regex_t *use_re, const cbm_regex_t *require_re,
                                   const cbm_regex_t *esimport_re, cbm_route_handler_t *routes,
                                   int route_count) {
    char *source = read_full_source(ctx, mod->file_path);
    if (!source) {
        return;
    }

    hl_import_entry_t imports[64];
    memset(imports, 0, sizeof(imports));
    int import_count = express_build_import_map(source, require_re, esimport_re, imports, 64);

    const char *p = source;
    cbm_regmatch_t pm[3];
    while (cbm_regexec(use_re, p, 3, pm, 0) == 0) {
        char prefix[256] = {0};
        char var_name[128] = {0};
        int plen = (pm[1].rm_eo - pm[1].rm_so);
        int vlen = (pm[2].rm_eo - pm[2].rm_so);
        if (plen < 256) {
            snprintf(prefix, 256, "%.*s", plen, p + pm[1].rm_so);
        }
        if (vlen < 128) {
            snprintf(var_name, 128, "%.*s", vlen, p + pm[2].rm_so);
        }
        p += pm[0].rm_eo;

        const char *module_path = NULL;
        for (int i = 0; i < import_count; i++) {
            if (strcmp(imports[i].var, var_name) == 0) {
                module_path = imports[i].module;
                break;
            }
        }
        if (!module_path) {
            continue;
        }

        express_apply_use_prefix(routes, route_count, module_path, prefix);
    }

    free(source);
}

/* Resolve Express app.use("/prefix", routerVar) prefixes. */
static void resolve_express_prefixes(cbm_pipeline_ctx_t *ctx, cbm_route_handler_t *routes,
                                     int route_count) {
    const cbm_gbuf_node_t **modules = NULL;
    int mod_count = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "Module", &modules, &mod_count) != 0) {
        return;
    }

    cbm_regex_t use_re;
    cbm_regex_t require_re;
    cbm_regex_t esimport_re;
    if (cbm_regcomp(
            &use_re,
            "\\.use\\([[:space:]]*[\"'`]([^\"'`]+)[\"'`][[:space:]]*,[[:space:]]*([[:alnum:]_]+)",
            CBM_REG_EXTENDED) != 0) {
        return;
    }
    if (cbm_regcomp(
            &require_re,
            "(const|let|var)[[:space:]]+([[:alnum:]_]+)[[:space:]]*=[[:space:]]*require\\([[:"
            "space:]]*[\"']([^\"']+)[\"']",
            CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&use_re);
        return;
    }
    if (cbm_regcomp(
            &esimport_re,
            "import[[:space:]]+([[:alnum:]_]+)[[:space:]]+from[[:space:]]+[\"']([^\"']+)[\"']",
            CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&use_re);
        cbm_regfree(&require_re);
        return;
    }

    for (int m = 0; m < mod_count; m++) {
        const cbm_gbuf_node_t *mod = modules[m];
        if (!mod->file_path || !is_jsts_file(mod->file_path)) {
            continue;
        }
        express_process_module(ctx, mod, &use_re, &require_re, &esimport_re, routes, route_count);
    }

    cbm_regfree(&use_re);
    cbm_regfree(&require_re);
    cbm_regfree(&esimport_re);
}

/* Apply a group prefix to all routes belonging to a function. */
static void apply_prefix_to_func_routes(cbm_route_handler_t *routes, int route_count,
                                        const char *func_qn, const char *prefix) {
    size_t pfx_len = strlen(prefix);
    char pfx_buf[256];
    snprintf(pfx_buf, sizeof(pfx_buf), "%s", prefix);
    while (pfx_len > 0 && pfx_buf[pfx_len - 1] == '/') {
        pfx_buf[--pfx_len] = '\0';
    }
    for (int r = 0; r < route_count; r++) {
        if (strcmp(routes[r].qualified_name, func_qn) != 0) {
            continue;
        }
        if (strncmp(routes[r].path, pfx_buf, pfx_len) == 0) {
            continue;
        }
        char new_path[256];
        const char *old = routes[r].path;
        while (*old == '/') {
            old++;
        }
        snprintf(new_path, sizeof(new_path), "%s/%s", pfx_buf, old);
        snprintf(routes[r].path, sizeof(routes[r].path), "%s", new_path);
    }
}

/* Index of routes grouped by function QN. */
typedef struct {
    const char *qn;
    int start;
    int count;
} hl_func_routes_t;

/* Build routesByFunc index: funcQN → (start_index, count) in routes array. */
static int build_func_route_index(const cbm_route_handler_t *routes, int route_count,
                                  hl_func_routes_t *func_map, int max_funcs) {
    int func_map_count = 0;
    for (int i = 0; i < route_count && func_map_count < max_funcs; i++) {
        int found = -1;
        for (int j = 0; j < func_map_count; j++) {
            if (strcmp(func_map[j].qn, routes[i].qualified_name) == 0) {
                found = j;
                func_map[j].count++;
                break;
            }
        }
        if (found < 0) {
            func_map[func_map_count].qn = routes[i].qualified_name;
            func_map[func_map_count].start = i;
            func_map[func_map_count].count = 1;
            func_map_count++;
        }
    }
    return func_map_count;
}

/* Group prefix var→prefix mapping. */
typedef struct {
    char var[128];
    char prefix[256];
} hl_var_prefix_t;

/* Try direct Group() pattern: FuncName(router.Group("/prefix")) */
static bool try_direct_group_prefix(const char *source, const cbm_regex_t *re, const char *name,
                                    cbm_route_handler_t *routes, int route_count,
                                    const char *func_qn) {
    cbm_regmatch_t pm[3];
    const char *p = source;
    while (cbm_regexec(re, p, 3, pm, 0) == 0) {
        char called_name[128] = {0};
        char prefix[256] = {0};
        int nlen = (pm[1].rm_eo - pm[1].rm_so);
        int plen = (pm[2].rm_eo - pm[2].rm_so);
        if (nlen < 128) {
            snprintf(called_name, 128, "%.*s", nlen, p + pm[1].rm_so);
        }
        if (plen < 256) {
            snprintf(prefix, 256, "%.*s", plen, p + pm[2].rm_so);
        }
        p += pm[0].rm_eo;
        if (strcmp(called_name, name) == 0) {
            apply_prefix_to_func_routes(routes, route_count, func_qn, prefix);
            return true;
        }
    }
    return false;
}

/* Collect var := r.Group("/prefix") assignments from source. */
static int collect_var_group_prefixes(const char *source, const cbm_regex_t *re,
                                      hl_var_prefix_t *out, int max_out) {
    cbm_regmatch_t pm[3];
    int count = 0;
    const char *p = source;
    while (count < max_out && cbm_regexec(re, p, 3, pm, 0) == 0) {
        int vlen = (pm[1].rm_eo - pm[1].rm_so);
        int plen = (pm[2].rm_eo - pm[2].rm_so);
        if (vlen < 128 && plen < 256) {
            snprintf(out[count].var, 128, "%.*s", vlen, p + pm[1].rm_so);
            snprintf(out[count].prefix, 256, "%.*s", plen, p + pm[2].rm_so);
            count++;
        }
        p += pm[0].rm_eo;
    }
    return count;
}

/* Try indirect Group() pattern: v1 := r.Group("/api"); FuncName(v1) */
static void try_indirect_group_prefix(const char *source, const cbm_gbuf_node_t *func_node,
                                      const hl_var_prefix_t *vars, int var_count,
                                      cbm_route_handler_t *routes, int route_count,
                                      const char *func_qn) {
    char call_pat[256];
    snprintf(call_pat, sizeof(call_pat), "%s[[:space:]]*\\([[:space:]]*([[:alnum:]_]+)",
             func_node->name);
    cbm_regex_t call_re;
    if (cbm_regcomp(&call_re, call_pat, CBM_REG_EXTENDED) != 0) {
        return;
    }
    cbm_regmatch_t pm[3];
    const char *p = source;
    while (cbm_regexec(&call_re, p, 2, pm, 0) == 0) {
        char arg_name[128] = {0};
        int alen = (pm[1].rm_eo - pm[1].rm_so);
        if (alen < 128) {
            snprintf(arg_name, 128, "%.*s", alen, p + pm[1].rm_so);
        }
        p += pm[0].rm_eo;
        for (int v = 0; v < var_count; v++) {
            if (strcmp(vars[v].var, arg_name) == 0) {
                apply_prefix_to_func_routes(routes, route_count, func_qn, vars[v].prefix);
                break;
            }
        }
    }
    cbm_regfree(&call_re);
}

static void resolve_group_from_caller(const char *caller_source, const cbm_gbuf_node_t *func_node,
                                      const cbm_regex_t *group_direct_re,
                                      const cbm_regex_t *group_var_re, cbm_route_handler_t *routes,
                                      int route_count, const char *func_qn) {
    if (try_direct_group_prefix(caller_source, group_direct_re, func_node->name, routes,
                                route_count, func_qn)) {
        return;
    }
    hl_var_prefix_t var_pfx[16];
    memset(var_pfx, 0, sizeof(var_pfx));
    int var_count = collect_var_group_prefixes(caller_source, group_var_re, var_pfx, 16);
    if (var_count > 0) {
        try_indirect_group_prefix(caller_source, func_node, var_pfx, var_count, routes, route_count,
                                  func_qn);
    }
}

/* Resolve Go gin cross-file Group() prefixes.
 * Pattern: v1 := r.Group("/api"); RegisterRoutes(v1) */
static void resolve_cross_file_group_prefixes(cbm_pipeline_ctx_t *ctx, cbm_route_handler_t *routes,
                                              int route_count) {
    hl_func_routes_t func_map[1024];
    memset(func_map, 0, sizeof(func_map));
    int func_map_count = build_func_route_index(routes, route_count, func_map, 1024);

    cbm_regex_t group_direct_re;
    cbm_regex_t group_var_re;
    if (cbm_regcomp(
            &group_direct_re,
            "([[:alnum:]_]+)\\([[:space:]]*[[:alnum:]_]+\\.Group\\([[:space:]]*\"([^\"]+)\"",
            CBM_REG_EXTENDED) != 0) {
        return;
    }
    if (cbm_regcomp(
            &group_var_re,
            "([[:alnum:]_]+)[[:space:]]*:?=[[:space:]]*[[:alnum:]_]+\\.Group\\([[:space:]]*\"(["
            "^\"]+)\"",
            CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&group_direct_re);
        return;
    }

    for (int fi = 0; fi < func_map_count; fi++) {
        const char *func_qn = func_map[fi].qn;
        const cbm_gbuf_node_t *func_node = cbm_gbuf_find_by_qn(ctx->gbuf, func_qn);
        if (!func_node) {
            continue;
        }

        const cbm_gbuf_edge_t **caller_edges = NULL;
        int caller_count = 0;
        if (cbm_gbuf_find_edges_by_target_type(ctx->gbuf, func_node->id, "CALLS", &caller_edges,
                                               &caller_count) != 0 ||
            caller_count == 0) {
            continue;
        }

        for (int ci = 0; ci < caller_count; ci++) {
            const cbm_gbuf_node_t *caller =
                cbm_gbuf_find_by_id(ctx->gbuf, caller_edges[ci]->source_id);
            if (!caller || !caller->file_path || caller->start_line <= 0) {
                continue;
            }

            char *caller_source =
                read_source_lines(ctx, caller->file_path, caller->start_line, caller->end_line);
            if (!caller_source) {
                continue;
            }

            resolve_group_from_caller(caller_source, func_node, &group_direct_re, &group_var_re,
                                      routes, route_count, func_qn);
            free(caller_source);
        }
    }

    cbm_regfree(&group_direct_re);
    cbm_regfree(&group_var_re);
}

/* ── Registration call edges ───────────────────────────────────── */

/* Create CALLS edges from route-registering functions to handler functions.
 * e.g., RegisterRoutes has .POST("/path", h.CreateOrder) → CALLS edge to CreateOrder. */
static int create_registration_call_edges(cbm_pipeline_ctx_t *ctx, cbm_route_handler_t *routes,
                                          int route_count) {
    int count = 0;
    for (int i = 0; i < route_count; i++) {
        if (routes[i].handler_ref[0] == '\0') {
            continue;
        }

        /* Find the registering function node */
        const cbm_gbuf_node_t *registrar = cbm_gbuf_find_by_qn(ctx->gbuf, routes[i].qualified_name);
        if (!registrar) {
            continue;
        }

        /* Resolve handler reference — strip receiver prefix (e.g., "h." from "h.CreateOrder") */
        const char *handler_name = routes[i].handler_ref;
        const char *dot = strrchr(handler_name, '.');
        if (dot) {
            handler_name = dot + 1;
        }

        /* Search for the handler function/method by name */
        const cbm_gbuf_node_t **handler_nodes = NULL;
        int handler_count = 0;
        if (cbm_gbuf_find_by_name(ctx->gbuf, handler_name, &handler_nodes, &handler_count) != 0) {
            continue;
        }
        if (handler_count == 0) {
            continue;
        }

        const cbm_gbuf_node_t *handler = handler_nodes[0];

        /* Store resolved handler QN for later use in insertRouteNodes */
        snprintf(routes[i].resolved_handler_qn, sizeof(routes[i].resolved_handler_qn), "%s",
                 handler->qualified_name);

        /* Create CALLS edge with via=route_registration */
        cbm_gbuf_insert_edge(ctx->gbuf, registrar->id, handler->id, "CALLS",
                             "{\"via\":\"route_registration\"}");
        count++;
    }
    return count;
}

/* ── Route node insertion ──────────────────────────────────────── */

/* Snapshot of handler node fields (gbuf pointers go stale after upsert). */
typedef struct {
    char file[512];
    char label[32];
    char name[256];
    char qn[512];
    char props_json[2048];
    int start;
    int end;
    int64_t id;
} hl_handler_snapshot_t;

/* Copy handler node fields into a stable snapshot. */
static void snapshot_handler_node(const cbm_gbuf_node_t *handler_node, hl_handler_snapshot_t *h) {
    memset(h, 0, sizeof(*h));
    snprintf(h->props_json, sizeof(h->props_json), "{}");
    if (!handler_node) {
        return;
    }
    if (handler_node->file_path) {
        snprintf(h->file, sizeof(h->file), "%s", handler_node->file_path);
    }
    if (handler_node->label) {
        snprintf(h->label, sizeof(h->label), "%s", handler_node->label);
    }
    if (handler_node->name) {
        snprintf(h->name, sizeof(h->name), "%s", handler_node->name);
    }
    if (handler_node->qualified_name) {
        snprintf(h->qn, sizeof(h->qn), "%s", handler_node->qualified_name);
    }
    if (handler_node->properties_json) {
        snprintf(h->props_json, sizeof(h->props_json), "%s", handler_node->properties_json);
    }
    h->start = handler_node->start_line;
    h->end = handler_node->end_line;
    h->id = handler_node->id;
}

/* Build Route node properties JSON with optional protocol detection. */
static void build_route_props(char *props, size_t props_size, const cbm_route_handler_t *rh,
                              const char *handler_qn, const hl_handler_snapshot_t *h,
                              const cbm_pipeline_ctx_t *ctx) {
    int n = snprintf(props, props_size, "{\"method\":\"%s\",\"path\":\"%s\",\"handler\":\"%s\"",
                     rh->method, rh->path, handler_qn);
    if (rh->protocol[0]) {
        n += snprintf(props + n, props_size - (size_t)n, ",\"protocol\":\"%s\"", rh->protocol);
    } else if (h->id > 0 && h->file[0] && h->start > 0) {
        char *hsource = read_source_lines(ctx, h->file, h->start, h->end);
        if (hsource) {
            const char *proto = cbm_detect_protocol(hsource);
            if (proto[0]) {
                n += snprintf(props + n, props_size - (size_t)n, ",\"protocol\":\"%s\"", proto);
            }
            free(hsource);
        }
    }
    snprintf(props + n, props_size - (size_t)n, "}");
}

/* Build normalized Route QN from method and path. */
static void build_route_qn(char *route_qn, size_t qn_size, const cbm_route_handler_t *rh) {
    char normal_method[16];
    snprintf(normal_method, sizeof(normal_method), "%s", rh->method[0] ? rh->method : "ANY");

    char normal_path[256];
    snprintf(normal_path, sizeof(normal_path), "%s", rh->path);
    for (char *c = normal_path; *c; c++) {
        if (*c == '/') {
            *c = '_';
        }
    }
    char *np = normal_path;
    while (*np == '_') {
        np++;
    }
    size_t nplen = strlen(np);
    while (nplen > 0 && np[nplen - 1] == '_') {
        np[--nplen] = '\0';
    }

    snprintf(route_qn, qn_size, "%s.route.%s.%s", rh->qualified_name, normal_method, np);
}

/* Insert Route nodes and HANDLES edges for discovered routes.
 * Uses ResolvedHandlerQN if set (from createRegistrationCallEdges). */
static int insert_route_nodes(cbm_pipeline_ctx_t *ctx, cbm_route_handler_t *routes,
                              int route_count) {
    int count = 0;
    for (int i = 0; i < route_count; i++) {
        cbm_route_handler_t *rh = &routes[i];

        char route_qn[1024];
        build_route_qn(route_qn, sizeof(route_qn), rh);

        char normal_method[16];
        snprintf(normal_method, sizeof(normal_method), "%s", rh->method[0] ? rh->method : "ANY");
        char route_name[256];
        snprintf(route_name, sizeof(route_name), "%s %s", normal_method, rh->path);

        const char *handler_qn = rh->qualified_name;
        if (rh->resolved_handler_qn[0]) {
            handler_qn = rh->resolved_handler_qn;
        }

        hl_handler_snapshot_t h;
        snapshot_handler_node(cbm_gbuf_find_by_qn(ctx->gbuf, handler_qn), &h);

        char props[512];
        build_route_props(props, sizeof(props), rh, handler_qn, &h, ctx);

        int64_t route_id = cbm_gbuf_upsert_node(ctx->gbuf, "Route", route_name, route_qn, h.file,
                                                h.start, h.end, props);
        if (route_id <= 0) {
            continue;
        }

        if (h.id > 0) {
            cbm_gbuf_insert_edge(ctx->gbuf, h.id, route_id, "HANDLES", "{}");

            char *new_props = set_entry_point(h.props_json);
            if (new_props) {
                cbm_gbuf_upsert_node(ctx->gbuf, h.label, h.name, h.qn, h.file, h.start, h.end,
                                     new_props);
                free(new_props);
            }
        }

        count++;
    }
    return count;
}

/* ── Match and link ────────────────────────────────────────────── */

/* Match one call site against all routes and create edges. Returns number of links created. */
static int match_site_to_routes(cbm_pipeline_ctx_t *ctx, const cbm_http_call_site_t *cs,
                                const cbm_gbuf_node_t *caller, const cbm_route_handler_t *routes,
                                int route_count) {
    int links = 0;
    for (int ri = 0; ri < route_count; ri++) {
        const cbm_route_handler_t *rh = &routes[ri];

        if (cbm_same_service(cs->source_qn, rh->qualified_name)) {
            continue;
        }
        if (cbm_is_path_excluded(rh->path, cbm_default_exclude_paths,
                                 cbm_default_exclude_paths_count)) {
            continue;
        }

        double score = cbm_path_match_score(cs->path, rh->path);
        if (score < MIN_PATH_CONFIDENCE) {
            continue;
        }

        double weight = (strcmp(cs->source_label, "Module") == 0) ? MODULE_WEIGHT : 1.0;
        score *= weight;
        if (score > 1.0) {
            score = 1.0;
        }

        const char *handler_qn = rh->qualified_name;
        if (rh->resolved_handler_qn[0]) {
            handler_qn = rh->resolved_handler_qn;
        }
        const cbm_gbuf_node_t *handler = cbm_gbuf_find_by_qn(ctx->gbuf, handler_qn);
        if (!handler) {
            continue;
        }

        const char *edge_type = cs->is_async ? "ASYNC_CALLS" : "HTTP_CALLS";
        const char *band = cbm_confidence_band(score);

        char edge_props[256];
        snprintf(edge_props, sizeof(edge_props),
                 "{\"url_path\":\"%s\",\"confidence\":%.3f,\"confidence_band\":\"%s\"}", cs->path,
                 score, band);

        cbm_gbuf_insert_edge(ctx->gbuf, caller->id, handler->id, edge_type, edge_props);
        links++;
    }
    return links;
}

/* Match call sites to routes and create HTTP_CALLS/ASYNC_CALLS edges. */
static int match_and_link(cbm_pipeline_ctx_t *ctx, cbm_route_handler_t *routes, int route_count,
                          cbm_http_call_site_t *sites, int site_count) {
    int link_count = 0;

    for (int si = 0; si < site_count; si++) {
        const cbm_http_call_site_t *cs = &sites[si];
        const cbm_gbuf_node_t *caller = cbm_gbuf_find_by_qn(ctx->gbuf, cs->source_qn);
        if (!caller) {
            continue;
        }
        link_count += match_site_to_routes(ctx, cs, caller, routes, route_count);
    }

    return link_count;
}

/* ── Parallel route discovery ──────────────────────────────────── */

/* Node entry for flat work array (tagged union of Function/Method/Module). */
typedef struct {
    const cbm_gbuf_node_t *node;
    bool is_module; /* true = Module, false = Function/Method */
} hl_work_item_t;

/* Per-worker buffer for route discovery. */
#define HL_ROUTES_PER_WORKER 512
typedef struct {
    cbm_route_handler_t routes[HL_ROUTES_PER_WORKER];
    int count;
} hl_route_buf_t;

/* Context for parallel route discovery. */
typedef struct {
    const hl_work_item_t *items;
    int item_count;
    const cbm_pipeline_ctx_t *ctx;
    hl_route_buf_t *worker_bufs; /* one per worker */
    int worker_count;
    _Atomic int next_idx;
    _Atomic int *cancelled;
} hl_route_ctx_t;

static void hl_route_worker(int worker_id, void *arg) {
    hl_route_ctx_t *rc = arg;
    hl_route_buf_t *buf = &rc->worker_bufs[worker_id];

    while (1) {
        int idx = atomic_fetch_add_explicit(&rc->next_idx, 1, memory_order_relaxed);
        if (idx >= rc->item_count) {
            break;
        }
        if (atomic_load_explicit(rc->cancelled, memory_order_relaxed)) {
            break;
        }

        const hl_work_item_t *item = &rc->items[idx];

        /* Skip test nodes */
        if (is_test_node(item->node)) {
            continue;
        }

        int space = HL_ROUTES_PER_WORKER - buf->count;
        if (space <= 0) {
            continue; /* worker buffer full */
        }

        int nr;
        if (item->is_module) {
            nr = discover_module_routes(item->node, rc->ctx, buf->routes + buf->count, space);
        } else {
            nr = discover_node_routes(item->node, rc->ctx, buf->routes + buf->count, space);
        }
        buf->count += nr;
    }
}

/* Per-worker buffer for call site discovery. */
#define HL_SITES_PER_WORKER 512
typedef struct {
    cbm_http_call_site_t sites[HL_SITES_PER_WORKER];
    int count;
} hl_site_buf_t;

/* Context for parallel call site discovery. */
typedef struct {
    const cbm_gbuf_node_t **nodes;
    const char **labels; /* "Function" or "Method" per node */
    int node_count;
    const cbm_pipeline_ctx_t *ctx;
    hl_site_buf_t *worker_bufs;
    int worker_count;
    _Atomic int next_idx;
    _Atomic int *cancelled;
} hl_site_ctx_t;

/* Check if source contains HTTP client or async dispatch keywords.
 * Returns: 0 = neither, 1 = http only, 2 = async only, 3 = both. */
static int check_http_async_keywords(const char *source) {
    bool has_http = false;
    for (int k = 0; k < cbm_http_client_keywords_count; k++) {
        if (strstr(source, cbm_http_client_keywords[k])) {
            has_http = true;
            break;
        }
    }
    bool has_async = false;
    for (int k = 0; k < cbm_async_dispatch_keywords_count; k++) {
        if (strstr(source, cbm_async_dispatch_keywords[k])) {
            has_async = true;
            break;
        }
    }
    return (has_http ? 1 : 0) | (has_async ? 2 : 0);
}

/* Return true if name looks like a Python dunder method (__xxx__). */
static bool is_dunder_method(const char *name) {
    if (!name) {
        return false;
    }
    size_t len = strlen(name);
    return len > 4 && name[0] == '_' && name[1] == '_' && name[len - 1] == '_' &&
           name[len - 2] == '_';
}

/* Store extracted URL paths as call sites in the worker buffer. */
static void store_site_paths(hl_site_buf_t *buf, const char *source, const cbm_gbuf_node_t *n,
                             const char *label, bool is_async) {
    char *paths[64];
    int path_count = cbm_extract_url_paths(source, paths, 64);

    int space = HL_SITES_PER_WORKER - buf->count;
    for (int p = 0; p < path_count && space > 0; p++) {
        cbm_http_call_site_t *site = &buf->sites[buf->count];
        snprintf(site->path, sizeof(site->path), "%s", paths[p]);
        site->method[0] = '\0';
        snprintf(site->source_name, sizeof(site->source_name), "%s", n->name);
        snprintf(site->source_qn, sizeof(site->source_qn), "%s", n->qualified_name);
        snprintf(site->source_label, sizeof(site->source_label), "%s", label);
        site->is_async = is_async;
        buf->count++;
        space--;
    }
    for (int p = 0; p < path_count; p++) {
        free(paths[p]);
    }
}

static void hl_site_worker(int worker_id, void *arg) {
    hl_site_ctx_t *sc = arg;
    hl_site_buf_t *buf = &sc->worker_bufs[worker_id];

    while (1) {
        int idx = atomic_fetch_add_explicit(&sc->next_idx, 1, memory_order_relaxed);
        if (idx >= sc->node_count) {
            break;
        }
        if (atomic_load_explicit(sc->cancelled, memory_order_relaxed)) {
            break;
        }

        const cbm_gbuf_node_t *n = sc->nodes[idx];
        if (!n->file_path || n->start_line <= 0 || n->end_line <= 0) {
            continue;
        }
        if (is_dunder_method(n->name)) {
            continue;
        }

        char *source = read_source_lines(sc->ctx, n->file_path, n->start_line, n->end_line);
        if (!source) {
            continue;
        }

        int kw = check_http_async_keywords(source);
        if (kw == 0) {
            free(source);
            continue;
        }

        bool is_async = (kw & 2) && !(kw & 1);
        store_site_paths(buf, source, n, sc->labels[idx], is_async);
        free(source);
    }
}

/* ── Main pass entry point ─────────────────────────────────────── */

/* Collect routes from Function/Method/Module nodes via parallel workers. */
static int collect_routes_parallel(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t **label_nodes[3],
                                   const int label_counts[3], int worker_count,
                                   cbm_route_handler_t *routes) {
    int total_route_nodes = 0;
    for (int li = 0; li < 3; li++) {
        total_route_nodes += label_counts[li];
    }

    hl_work_item_t *work_items = NULL;
    if (total_route_nodes > 0) {
        work_items = malloc((size_t)total_route_nodes * sizeof(hl_work_item_t));
    }

    int wi = 0;
    if (work_items) {
        for (int li = 0; li < 3; li++) {
            for (int i = 0; i < label_counts[li]; i++) {
                work_items[wi].node = label_nodes[li][i];
                work_items[wi].is_module = (li == 2);
                wi++;
            }
        }
    }

    int route_count = 0;
    hl_route_buf_t *route_bufs = calloc((size_t)worker_count, sizeof(hl_route_buf_t));

    if (work_items && route_bufs && wi > 0) {
        hl_route_ctx_t rc = {
            .items = work_items,
            .item_count = wi,
            .ctx = ctx,
            .worker_bufs = route_bufs,
            .worker_count = worker_count,
            .cancelled = ctx->cancelled,
        };
        atomic_init(&rc.next_idx, 0);

        cbm_parallel_for_opts_t opts = {.max_workers = worker_count, .force_pthreads = false};
        cbm_parallel_for(worker_count, hl_route_worker, &rc, opts);

        for (int w = 0; w < worker_count; w++) {
            int to_copy = route_bufs[w].count;
            if (to_copy > MAX_ROUTES - route_count) {
                to_copy = MAX_ROUTES - route_count;
            }
            if (to_copy > 0) {
                memcpy(routes + route_count, route_bufs[w].routes,
                       (size_t)to_copy * sizeof(cbm_route_handler_t));
                route_count += to_copy;
            }
        }
    }
    free(work_items);
    free(route_bufs);
    return route_count;
}

/* Collect HTTP/async call sites from Function/Method nodes via parallel workers. */
static int collect_sites_parallel(const cbm_pipeline_ctx_t *ctx,
                                  const cbm_gbuf_node_t **label_nodes[3], const int label_counts[3],
                                  int worker_count, cbm_http_call_site_t *sites) {
    const char *site_labels[] = {"Function", "Method"};
    int total_site_nodes = 0;
    for (int li = 0; li < 2; li++) {
        total_site_nodes += label_counts[li];
    }
    if (total_site_nodes == 0) {
        return 0;
    }

    const cbm_gbuf_node_t **all_site_nodes =
        malloc((size_t)total_site_nodes * sizeof(cbm_gbuf_node_t *));
    const char **all_site_labels = malloc((size_t)total_site_nodes * sizeof(const char *));
    if (!all_site_nodes || !all_site_labels) {
        free(all_site_nodes);
        free(all_site_labels);
        return 0;
    }

    int si2 = 0;
    for (int li = 0; li < 2; li++) {
        for (int i = 0; i < label_counts[li]; i++) {
            all_site_nodes[si2] = label_nodes[li][i];
            all_site_labels[si2] = site_labels[li];
            si2++;
        }
    }

    int site_count = 0;
    hl_site_buf_t *site_bufs = calloc((size_t)worker_count, sizeof(hl_site_buf_t));
    if (site_bufs && si2 > 0) {
        hl_site_ctx_t sc = {
            .nodes = all_site_nodes,
            .labels = all_site_labels,
            .node_count = si2,
            .ctx = ctx,
            .worker_bufs = site_bufs,
            .worker_count = worker_count,
            .cancelled = ctx->cancelled,
        };
        atomic_init(&sc.next_idx, 0);

        cbm_parallel_for_opts_t opts = {.max_workers = worker_count, .force_pthreads = false};
        cbm_parallel_for(worker_count, hl_site_worker, &sc, opts);

        for (int w = 0; w < worker_count; w++) {
            int to_copy = site_bufs[w].count;
            if (to_copy > MAX_CALL_SITES - site_count) {
                to_copy = MAX_CALL_SITES - site_count;
            }
            if (to_copy > 0) {
                memcpy(sites + site_count, site_bufs[w].sites,
                       (size_t)to_copy * sizeof(cbm_http_call_site_t));
                site_count += to_copy;
            }
        }
    }
    free(site_bufs);
    free(all_site_nodes);
    free(all_site_labels);
    return site_count;
}

int cbm_pipeline_pass_httplinks(cbm_pipeline_ctx_t *ctx) {
    cbm_log_info("pass.start", "pass", "httplinks");

    if (cbm_pipeline_check_cancel(ctx)) {
        return -1;
    }

    int worker_count = cbm_default_worker_count(true);
    if (worker_count < 1) {
        worker_count = 1;
    }

    /* ── Phase 1: Route collection from decorator properties + disk ── */
    const char *route_labels[] = {"Function", "Method", "Module"};
    const cbm_gbuf_node_t **label_nodes[3] = {NULL, NULL, NULL};
    int label_counts[3] = {0, 0, 0};

    for (int li = 0; li < 3; li++) {
        cbm_gbuf_find_by_label(ctx->gbuf, route_labels[li], &label_nodes[li], &label_counts[li]);
    }

    cbm_route_handler_t *routes = calloc(MAX_ROUTES, sizeof(cbm_route_handler_t));
    if (!routes) {
        return -1;
    }
    int route_count = collect_routes_parallel(ctx, label_nodes, label_counts, worker_count, routes);
    cbm_log_info("httplink.routes", "count", itoa_hl(route_count));

    /* ── Phase 2: Resolve cross-file prefixes (serial) ────────── */
    resolve_cross_file_group_prefixes(ctx, routes, route_count);
    resolve_fastapi_prefixes(ctx, routes, route_count);
    resolve_express_prefixes(ctx, routes, route_count);

    /* ── Phase 3: Registration edges (serial) ─────────────────── */
    int reg_edges = create_registration_call_edges(ctx, routes, route_count);
    if (reg_edges > 0) {
        cbm_log_info("httplink.registration_edges", "count", itoa_hl(reg_edges));
    }

    /* ── Phase 4: Route nodes + HANDLES edges (serial) ────────── */
    int route_nodes = insert_route_nodes(ctx, routes, route_count);

    /* ── Phase 5: Call site collection ──────────────────────────── */
    cbm_http_call_site_t *sites = calloc(MAX_CALL_SITES, sizeof(cbm_http_call_site_t));
    int site_count = 0;
    if (sites) {
        site_count = collect_sites_parallel(ctx, label_nodes, label_counts, worker_count, sites);
    }
    cbm_log_info("httplink.callsites", "count", itoa_hl(site_count));

    /* ── Phase 6: Match and link (serial) ─────────────────────── */
    int link_count = 0;
    if (sites && site_count > 0 && route_count > 0) {
        link_count = match_and_link(ctx, routes, route_count, sites, site_count);
    }

    free(routes);
    free(sites);

    cbm_log_info("pass.done", "pass", "httplinks", "routes", itoa_hl(route_nodes), "calls",
                 itoa_hl(link_count));
    return 0;
}
