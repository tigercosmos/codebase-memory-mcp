/*
 * pass_semantic_edges.c — Emit SEMANTICALLY_RELATED edges from combined
 * algorithmic embeddings (11 signals, zero external dependencies).
 *
 * Runs as a post-pass after pass_similarity. Reads all Function/Method
 * nodes from the graph buffer, computes TF-IDF + Random Indexing + API
 * signatures + type/decorator vectors + AST profile, builds LSH index,
 * scores candidate pairs, applies graph diffusion, emits edges.
 *
 * Runs in moderate and full modes (not fast). Controlled by pipeline mode.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include <stdint.h>
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "semantic/semantic.h"
#include "semantic/ast_profile.h"
#include "simhash/minhash.h"
#include "foundation/log.h"
#include "foundation/compat.h"
#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#include "pipeline/worker_pool.h"
#include "foundation/platform.h"
#include "foundation/profile.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

enum { PROPS_BUF = 512, MAX_FUNCS_INIT = 4096, GROW = 2, MAX_CALLEES = 64,
       MAX_DEFERRED_EDGES = 8192 };

/* ── Deferred edge buffer (thread-local, merged after parallel pass) ── */

typedef struct {
    int64_t source_id;
    int64_t target_id;
    float score;
    bool same_file;
} deferred_edge_t;

typedef struct {
    deferred_edge_t *edges;
    int count;
    int cap;
} deferred_edge_buf_t;

static void deferred_buf_init(deferred_edge_buf_t *buf) {
    buf->edges = NULL;
    buf->count = 0;
    buf->cap = 0;
}

static void deferred_buf_push(deferred_edge_buf_t *buf, int64_t src, int64_t tgt,
                               float score, bool same_file) {
    if (buf->count >= buf->cap) {
        int nc = buf->cap < CBM_SZ_256 ? CBM_SZ_256 : buf->cap * GROW;
        deferred_edge_t *grown = realloc(buf->edges, (size_t)nc * sizeof(deferred_edge_t));
        if (!grown) return;
        buf->edges = grown;
        buf->cap = nc;
    }
    buf->edges[buf->count++] = (deferred_edge_t){
        .source_id = src, .target_id = tgt, .score = score, .same_file = same_file
    };
}

static void deferred_buf_free(deferred_edge_buf_t *buf) {
    free(buf->edges);
    buf->edges = NULL;
    buf->count = buf->cap = 0;
}

/* Forward declare helpers used by pattern injection. */
static const char *json_str_value(const char *json, const char *key, char *buf, int bufsize);

/* ── Technique 2: Code pattern vocabulary injection ──────────────── */
/* Inject semantic tokens based on detected code patterns.
 * This bridges the vocabulary gap for abstract concepts like "error handling". */

static int inject_pattern_tokens(const cbm_gbuf_node_t *n, const cbm_gbuf_t *gbuf, char **tokens,
                                 int count, int max_tokens) {
    if (!n || count >= max_tokens) {
        return count;
    }

    /* Check body tokens for patterns */
    char bt_buf[CBM_SZ_512];
    const char *bt = n->properties_json ? json_str_value(n->properties_json, "bt", bt_buf,
                                                         sizeof(bt_buf))
                                        : NULL;
    /* Check decorators */
    char dec_buf[CBM_SZ_256];
    const char *decs = n->properties_json ? json_str_value(n->properties_json, "decorators", dec_buf,
                                                           sizeof(dec_buf))
                                          : NULL;

    /* Pattern: try/catch/except in body → inject error handling tokens */
    if (bt && (strstr(bt, "except") || strstr(bt, "catch") || strstr(bt, "rescue"))) {
        if (count < max_tokens) tokens[count++] = strdup("error");
        if (count < max_tokens) tokens[count++] = strdup("handling");
        if (count < max_tokens) tokens[count++] = strdup("exception");
    }

    /* Pattern: raise/throw in body → inject error tokens */
    if (bt && (strstr(bt, "raise") || strstr(bt, "throw"))) {
        if (count < max_tokens) tokens[count++] = strdup("error");
        if (count < max_tokens) tokens[count++] = strdup("exception");
        if (count < max_tokens) tokens[count++] = strdup("throw");
    }

    /* Pattern: log/logger in callees or body → inject logging tokens */
    if (bt && (strstr(bt, "logger") || strstr(bt, "logging") || strstr(bt, "log_"))) {
        if (count < max_tokens) tokens[count++] = strdup("logging");
        if (count < max_tokens) tokens[count++] = strdup("log");
    }

    /* Check callee names for logging/error patterns */
    if (gbuf) {
        const cbm_gbuf_edge_t **edges = NULL;
        int ec = 0;
        if (cbm_gbuf_find_edges_by_source_type(gbuf, n->id, "CALLS", &edges, &ec) == 0) {
            for (int e = 0; e < ec && count < max_tokens; e++) {
                const cbm_gbuf_node_t *t = cbm_gbuf_find_by_id(gbuf, edges[e]->target_id);
                if (!t || !t->name) continue;
                /* Callee is a logging function */
                if (strstr(t->name, "log") || strstr(t->name, "Log") || strstr(t->name, "warn") ||
                    strstr(t->name, "debug") || strstr(t->name, "info")) {
                    if (count < max_tokens) tokens[count++] = strdup("logging");
                    if (count < max_tokens) tokens[count++] = strdup("log");
                }
                /* Callee is an error function */
                if (strstr(t->name, "Error") || strstr(t->name, "error") ||
                    strstr(t->name, "Errorf") || strstr(t->name, "panic")) {
                    if (count < max_tokens) tokens[count++] = strdup("error");
                    if (count < max_tokens) tokens[count++] = strdup("handling");
                }
                /* Callee is a file/IO function */
                if (strstr(t->name, "open") || strstr(t->name, "read") || strstr(t->name, "write") ||
                    strstr(t->name, "close") || strstr(t->name, "Open") || strstr(t->name, "Read")) {
                    if (count < max_tokens) tokens[count++] = strdup("io");
                    if (count < max_tokens) tokens[count++] = strdup("file");
                }
            }
        }
    }

    /* Pattern: decorator-based injection */
    if (decs) {
        if (strstr(decs, "route") || strstr(decs, "Route") || strstr(decs, "app.")) {
            if (count < max_tokens) tokens[count++] = strdup("routing");
            if (count < max_tokens) tokens[count++] = strdup("endpoint");
            if (count < max_tokens) tokens[count++] = strdup("handler");
        }
        if (strstr(decs, "middleware") || strstr(decs, "Middleware")) {
            if (count < max_tokens) tokens[count++] = strdup("middleware");
        }
        if (strstr(decs, "test") || strstr(decs, "Test") || strstr(decs, "pytest")) {
            if (count < max_tokens) tokens[count++] = strdup("test");
            if (count < max_tokens) tokens[count++] = strdup("testing");
        }
    }

    /* Pattern: name-based injection */
    if (n->name) {
        if (strstr(n->name, "test_") || strstr(n->name, "Test")) {
            if (count < max_tokens) tokens[count++] = strdup("test");
            if (count < max_tokens) tokens[count++] = strdup("testing");
        }
        if (strstr(n->name, "middleware") || strstr(n->name, "Middleware")) {
            if (count < max_tokens) tokens[count++] = strdup("middleware");
        }
        if (strstr(n->name, "handler") || strstr(n->name, "Handler")) {
            if (count < max_tokens) tokens[count++] = strdup("handler");
        }
        if (strstr(n->name, "validator") || strstr(n->name, "Validator") ||
            strstr(n->name, "validate") || strstr(n->name, "Validate")) {
            if (count < max_tokens) tokens[count++] = strdup("validation");
        }
    }

    return count;
}

/* ── Technique 3: Field weights for token sources ────────────────── */
enum {
    FW_NAME = 30,     /* ×3.0 */
    FW_CALLEE = 20,   /* ×2.0 */
    FW_BODY = 15,     /* ×1.5 */
    FW_SIGNATURE = 10, /* ×1.0 */
    FW_PARAM = 10,    /* ×1.0 */
    FW_PATTERN = 25,  /* ×2.5 — injected semantic tokens are high value */
    FW_PATH = 5,      /* ×0.5 */
    FW_SCALE = 10,    /* divisor */
};

static const char *itoa_log(int val) {
    enum { RING = 4, MASK = 3 };
    static CBM_TLS char bufs[RING][CBM_SZ_32];
    static CBM_TLS int idx = 0;
    int i = idx;
    idx = (idx + SKIP_ONE) & MASK;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

static const char *file_ext(const char *path) {
    if (!path) {
        return "";
    }
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

/* Extract a JSON string value by key (simple strstr-based, no full parse). */
static const char *json_str_value(const char *json, const char *key, char *buf, int bufsize) {
    if (!json || !key) {
        return NULL;
    }
    char search[CBM_SZ_64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *start = strstr(json, search);
    if (!start) {
        return NULL;
    }
    start += strlen(search);
    const char *end = strchr(start, '"');
    if (!end) {
        return NULL;
    }
    int len = (int)(end - start);
    if (len >= bufsize) {
        len = bufsize - SKIP_ONE;
    }
    memcpy(buf, start, (size_t)len);
    buf[len] = '\0';
    return buf;
}

/* Extract a JSON array of strings by key. Returns count. */
static int json_str_array(const char *json, const char *key, char **out, int max_out) {
    if (!json || !key) {
        return 0;
    }
    char search[CBM_SZ_64];
    snprintf(search, sizeof(search), "\"%s\":[", key);
    const char *start = strstr(json, search);
    if (!start) {
        return 0;
    }
    start += strlen(search);
    int count = 0;
    while (*start && *start != ']' && count < max_out) {
        if (*start == '"') {
            start++;
            const char *end = strchr(start, '"');
            if (!end) {
                break;
            }
            int len = (int)(end - start);
            out[count] = malloc((size_t)len + SKIP_ONE);
            memcpy(out[count], start, (size_t)len);
            out[count][len] = '\0';
            count++;
            start = end + SKIP_ONE;
        } else {
            start++;
        }
    }
    return count;
}

/* ── Tokenize node metadata ──────────────────────────────────────── */

static int tokenize_node(const cbm_gbuf_node_t *n, const cbm_gbuf_t *gbuf, char **tokens,
                         int max_tokens) {
    int count = 0;
    /* Tokenize name (primary signal) */
    count += cbm_sem_tokenize(n->name, tokens + count, max_tokens - count);
    /* Tokenize qualified name path components */
    if (n->qualified_name && count < max_tokens) {
        count += cbm_sem_tokenize(n->qualified_name, tokens + count, max_tokens - count);
    }
    /* Tokenize file path (directory = domain context) */
    if (n->file_path && count < max_tokens) {
        count += cbm_sem_tokenize(n->file_path, tokens + count, max_tokens - count);
    }
    /* Extract rich metadata from properties_json */
    if (n->properties_json && count < max_tokens) {
        char buf[CBM_SZ_512];
        /* Signature (parameter list text) */
        if (json_str_value(n->properties_json, "signature", buf, sizeof(buf))) {
            count += cbm_sem_tokenize(buf, tokens + count, max_tokens - count);
        }
        /* Return type */
        if (count < max_tokens &&
            json_str_value(n->properties_json, "return_type", buf, sizeof(buf))) {
            count += cbm_sem_tokenize(buf, tokens + count, max_tokens - count);
        }
        /* Docstring (natural language — rich vocabulary for co-occurrence) */
        if (count < max_tokens &&
            json_str_value(n->properties_json, "docstring", buf, sizeof(buf))) {
            count += cbm_sem_tokenize(buf, tokens + count, max_tokens - count);
        }
        /* Param names */
        char *pnames[CBM_SZ_16];
        int pn_count = json_str_array(n->properties_json, "param_names", pnames, CBM_SZ_16);
        for (int p = 0; p < pn_count && count < max_tokens; p++) {
            count += cbm_sem_tokenize(pnames[p], tokens + count, max_tokens - count);
            free(pnames[p]);
        }
        /* Param types */
        char *ptypes[CBM_SZ_16];
        int pt_count = json_str_array(n->properties_json, "param_types", ptypes, CBM_SZ_16);
        for (int p = 0; p < pt_count && count < max_tokens; p++) {
            count += cbm_sem_tokenize(ptypes[p], tokens + count, max_tokens - count);
            free(ptypes[p]);
        }
        /* Decorators */
        char *decos[CBM_SZ_16];
        int dc = json_str_array(n->properties_json, "decorators", decos, CBM_SZ_16);
        for (int p = 0; p < dc && count < max_tokens; p++) {
            count += cbm_sem_tokenize(decos[p], tokens + count, max_tokens - count);
            free(decos[p]);
        }
    }

    /* Body tokens: raw identifiers from function body AST.
     * Captures what the function DOES — "error" in catch blocks, "log" in logging, etc. */
    if (n->properties_json && count < max_tokens) {
        char bt_buf[CBM_SZ_512];
        if (json_str_value(n->properties_json, "bt", bt_buf, sizeof(bt_buf))) {
            count += cbm_sem_tokenize(bt_buf, tokens + count, max_tokens - count);
        }
    }

    /* Callee names: what this function CALLS (behavioral vocabulary).
     * "error handling" functions call errors.New, log.Error, etc. */
    if (gbuf && count < max_tokens) {
        const cbm_gbuf_edge_t **call_edges = NULL;
        int call_count = 0;
        if (cbm_gbuf_find_edges_by_source_type(gbuf, n->id, "CALLS", &call_edges, &call_count) ==
            0) {
            for (int e = 0; e < call_count && count < max_tokens; e++) {
                const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, call_edges[e]->target_id);
                if (target && target->name) {
                    count += cbm_sem_tokenize(target->name, tokens + count, max_tokens - count);
                }
            }
        }
    }

    /* Caller names: what CALLS this function (contextual vocabulary).
     * Functions called by error handlers inherit "error" context. */
    if (gbuf && count < max_tokens) {
        const cbm_gbuf_edge_t **caller_edges = NULL;
        int caller_count = 0;
        if (cbm_gbuf_find_edges_by_target_type(gbuf, n->id, "CALLS", &caller_edges,
                                                &caller_count) == 0) {
            for (int e = 0; e < caller_count && e < MAX_CALLEES && count < max_tokens; e++) {
                const cbm_gbuf_node_t *source =
                    cbm_gbuf_find_by_id(gbuf, caller_edges[e]->source_id);
                if (source && source->name) {
                    count += cbm_sem_tokenize(source->name, tokens + count, max_tokens - count);
                }
            }
        }
    }

    return count;
}

/* ── Build per-function semantic data ────────────────────────────── */

static void build_api_vec(const cbm_gbuf_t *gbuf, int64_t node_id, cbm_sem_vec_t *out) {
    memset(out, 0, sizeof(*out));
    const cbm_gbuf_edge_t **edges = NULL;
    int edge_count = 0;
    if (cbm_gbuf_find_edges_by_source_type(gbuf, node_id, "CALLS", &edges, &edge_count) != 0) {
        return;
    }
    int added = 0;
    for (int i = 0; i < edge_count && added < MAX_CALLEES; i++) {
        const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, edges[i]->target_id);
        if (target && target->name) {
            cbm_sem_vec_t callee_ri;
            cbm_sem_random_index(target->name, &callee_ri);
            cbm_sem_vec_add_scaled(out, &callee_ri, 1.0f);
            added++;
        }
    }
    cbm_sem_normalize(out);
}

static void build_type_vec(const char *props_json, cbm_sem_vec_t *out) {
    memset(out, 0, sizeof(*out));
    if (!props_json) {
        return;
    }
    /* Extract param_types and return_type */
    char rt_buf[CBM_SZ_128];
    if (json_str_value(props_json, "return_type", rt_buf, sizeof(rt_buf))) {
        cbm_sem_vec_t ri;
        cbm_sem_random_index(rt_buf, &ri);
        cbm_sem_vec_add_scaled(out, &ri, 1.0f);
    }
    char *ptypes[CBM_SZ_16];
    int pt_count = json_str_array(props_json, "param_types", ptypes, CBM_SZ_16);
    for (int i = 0; i < pt_count; i++) {
        cbm_sem_vec_t ri;
        cbm_sem_random_index(ptypes[i], &ri);
        cbm_sem_vec_add_scaled(out, &ri, 1.0f);
        free(ptypes[i]);
    }
    cbm_sem_normalize(out);
}

static void build_deco_vec(const char *props_json, cbm_sem_vec_t *out) {
    memset(out, 0, sizeof(*out));
    if (!props_json) {
        return;
    }
    char *decos[CBM_SZ_16];
    int dc = json_str_array(props_json, "decorators", decos, CBM_SZ_16);
    for (int i = 0; i < dc; i++) {
        cbm_sem_vec_t ri;
        cbm_sem_random_index(decos[i], &ri);
        cbm_sem_vec_add_scaled(out, &ri, 1.0f);
        free(decos[i]);
    }
    cbm_sem_normalize(out);
}

static void decode_struct_profile(const char *props_json, float *out) {
    memset(out, 0, sizeof(float) * CBM_AST_PROFILE_DIMS);
    if (!props_json) {
        return;
    }
    char sp_buf[CBM_AST_PROFILE_BUF];
    if (json_str_value(props_json, "sp", sp_buf, sizeof(sp_buf))) {
        cbm_ast_profile_t profile;
        if (cbm_ast_profile_from_str(sp_buf, &profile)) {
            cbm_ast_profile_to_vector(&profile, out);
        }
    }
}

static void decode_minhash(const char *props_json, cbm_sem_func_t *func) {
    func->has_minhash = false;
    if (!props_json) {
        return;
    }
    const char *fp_key = strstr(props_json, "\"fp\":\"");
    if (!fp_key) {
        return;
    }
    const char *hex = fp_key + 6; /* strlen("\"fp\":\"") */
    const char *end = strchr(hex, '"');
    if (!end || (int)(end - hex) != CBM_MINHASH_HEX_LEN) {
        return;
    }
    char hex_buf[CBM_MINHASH_HEX_BUF];
    memcpy(hex_buf, hex, CBM_MINHASH_HEX_LEN);
    hex_buf[CBM_MINHASH_HEX_LEN] = '\0';
    cbm_minhash_t mh;
    if (cbm_minhash_from_hex(hex_buf, &mh)) {
        memcpy(func->minhash, mh.values, sizeof(func->minhash));
        func->has_minhash = true;
    }
}

/* ── Parallel Phase 2: Tokenize nodes ────────────────────────────── */

typedef struct {
    const cbm_gbuf_node_t **node_ptrs; /* node pointer per function index */
    cbm_gbuf_t *gbuf;                  /* read-only during tokenization */
    char **all_tokens;                  /* output: all_tokens[f * MAX + t] */
    int *token_counts;                  /* output: token count per function */
    int func_count;
    _Atomic int next_idx;
} tokenize_ctx_t;

static void tokenize_worker(int worker_id, void *ctx_ptr) {
    (void)worker_id;
    tokenize_ctx_t *tc = ctx_ptr;
    while (1) {
        int f = atomic_fetch_add_explicit(&tc->next_idx, 1, memory_order_relaxed);
        if (f >= tc->func_count) break;

        const cbm_gbuf_node_t *n = tc->node_ptrs[f];
        char *tokens[CBM_SEM_MAX_TOKENS];
        int count = tokenize_node(n, tc->gbuf, tokens, CBM_SEM_MAX_TOKENS);
        count = inject_pattern_tokens(n, tc->gbuf, tokens, count, CBM_SEM_MAX_TOKENS);
        tc->token_counts[f] = count;
        for (int t = 0; t < count; t++) {
            tc->all_tokens[f * CBM_SEM_MAX_TOKENS + t] = tokens[t];
        }
    }
}

/* ── Parallel Phase 4: Build per-function vectors ────────────────── */

typedef struct {
    cbm_sem_func_t *funcs;
    char **all_tokens;
    int *token_counts;
    cbm_sem_corpus_t *corpus;
    uint8_t *qvecs;           /* output: pre-quantized int8 vectors [func_count * CBM_SEM_DIM] */
    int func_count;
    _Atomic int next_idx;
} vec_build_ctx_t;

static void vec_build_worker(int worker_id, void *ctx_ptr) {
    (void)worker_id;
    vec_build_ctx_t *vc = ctx_ptr;
    while (1) {
        int f = atomic_fetch_add_explicit(&vc->next_idx, 1, memory_order_relaxed);
        if (f >= vc->func_count) break;

        int tc = vc->token_counts[f];
        char **tokens = &vc->all_tokens[f * CBM_SEM_MAX_TOKENS];

        /* TF-IDF weights */
        int *indices = malloc((size_t)tc * sizeof(int));
        float *weights = malloc((size_t)tc * sizeof(float));
        int tfidf_len = 0;
        for (int t = 0; t < tc; t++) {
            float idf = cbm_sem_corpus_idf(vc->corpus, tokens[t]);
            if (idf > 0.0f) {
                indices[tfidf_len] = t;
                weights[tfidf_len] = idf;
                tfidf_len++;
            }
        }
        vc->funcs[f].tfidf_indices = indices;
        vc->funcs[f].tfidf_weights = weights;
        vc->funcs[f].tfidf_len = tfidf_len;

        /* RI vector: sum of enriched token vectors weighted by IDF */
        memset(&vc->funcs[f].ri_vec, 0, sizeof(cbm_sem_vec_t));
        for (int t = 0; t < tc; t++) {
            const cbm_sem_vec_t *ri = cbm_sem_corpus_ri_vec(vc->corpus, tokens[t]);
            if (ri) {
                float idf = cbm_sem_corpus_idf(vc->corpus, tokens[t]);
                cbm_sem_vec_add_scaled(&vc->funcs[f].ri_vec, ri, idf);
            }
        }
        cbm_sem_normalize(&vc->funcs[f].ri_vec);

        /* Int8 quantize into pre-allocated output array (parallel-safe) */
        uint8_t *qv = &vc->qvecs[f * CBM_SEM_DIM];
        for (int d = 0; d < CBM_SEM_DIM; d++) {
            float v = vc->funcs[f].ri_vec.v[d];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            qv[d] = (uint8_t)(int8_t)(v * 127.0f);
        }
    }
}

/* ── Parallel Phase 5: LSH signatures ────────────────────────────── */

enum {
    NUM_HYPERPLANES = 64,
    SEM_LSH_BANDS = 16,
    SEM_LSH_ROWS = 4,
    SEM_BUCKET_COUNT = 65536,
    SEM_BUCKET_MASK = 65535,
    SEM_BUCKET_CAP_INIT = 16,
    SEM_MAX_CANDIDATES = 200,
};

typedef struct {
    cbm_sem_func_t *funcs;
    uint64_t *signatures;
    float (*hyperplanes)[CBM_SEM_DIM];
    int func_count;
    _Atomic int next_idx;
} sig_build_ctx_t;

static void sig_build_worker(int worker_id, void *ctx_ptr) {
    (void)worker_id;
    sig_build_ctx_t *sc = ctx_ptr;
    while (1) {
        int f = atomic_fetch_add_explicit(&sc->next_idx, 1, memory_order_relaxed);
        if (f >= sc->func_count) break;

        uint64_t sig = 0;
        for (int h = 0; h < NUM_HYPERPLANES; h++) {
            float dot = 0.0f;
            for (int d = 0; d < CBM_SEM_DIM; d++) {
                dot += sc->funcs[f].ri_vec.v[d] * sc->hyperplanes[h][d];
            }
            if (dot > 0.0f) {
                sig |= (1ULL << h);
            }
        }
        sc->signatures[f] = sig;
    }
}

/* ── Parallel Phase 6: Score candidates + collect edges ──────────── */

typedef struct {
    cbm_sem_func_t *funcs;
    uint64_t *signatures;
    int *edge_counts;         /* shared, atomically updated */
    cbm_sem_config_t cfg;
    int func_count;

    /* LSH buckets (read-only during scoring) */
    struct { int *items; int count; int cap; } **band_buckets;

    /* Per-worker edge buffer */
    deferred_edge_buf_t *worker_bufs;
    int max_workers;
    _Atomic int next_idx;
} score_ctx_t;

static void score_worker(int worker_id, void *ctx_ptr) {
    score_ctx_t *sc = ctx_ptr;
    deferred_edge_buf_t *my_buf = &sc->worker_bufs[worker_id];

    enum { SEEN_CAP = 8192, SEEN_MASK = 8191 };

    while (1) {
        int i = atomic_fetch_add_explicit(&sc->next_idx, 1, memory_order_relaxed);
        if (i >= sc->func_count) break;

        int my_edges = atomic_load_explicit((_Atomic int *)&sc->edge_counts[i],
                                             memory_order_relaxed);
        if (my_edges >= sc->cfg.max_edges) continue;

        /* Collect unique candidates from all bands */
        int seen[SEEN_CAP];
        memset(seen, -1, sizeof(seen));
        int candidates[SEM_MAX_CANDIDATES];
        int cand_count = 0;

        for (int b = 0; b < SEM_LSH_BANDS && cand_count < SEM_MAX_CANDIDATES; b++) {
            int shift = b * SEM_LSH_ROWS;
            uint32_t band_val = (uint32_t)((sc->signatures[i] >> shift) &
                                            ((1ULL << SEM_LSH_ROWS) - 1));
            uint64_t bh = XXH3_64bits_withSeed(&band_val, sizeof(band_val), (uint64_t)b);
            uint32_t bucket_idx = (uint32_t)(bh & SEM_BUCKET_MASK);
            int bcount = sc->band_buckets[b][bucket_idx].count;
            int *bitems = sc->band_buckets[b][bucket_idx].items;

            if (bcount > SEM_MAX_CANDIDATES) continue;

            for (int k = 0; k < bcount && cand_count < SEM_MAX_CANDIDATES; k++) {
                int j = bitems[k];
                if (j <= i) continue;
                uint32_t slot = (uint32_t)j & SEEN_MASK;
                bool dup = false;
                for (int p = 0; p < SEEN_CAP; p++) {
                    uint32_t idx = (slot + (uint32_t)p) & SEEN_MASK;
                    if (seen[idx] == -1) { seen[idx] = j; break; }
                    if (seen[idx] == j) { dup = true; break; }
                }
                if (dup) continue;
                candidates[cand_count++] = j;
            }
        }

        /* Score candidates */
        for (int c = 0; c < cand_count; c++) {
            int j = candidates[c];
            int ei = atomic_load_explicit((_Atomic int *)&sc->edge_counts[i],
                                           memory_order_relaxed);
            int ej = atomic_load_explicit((_Atomic int *)&sc->edge_counts[j],
                                           memory_order_relaxed);
            if (ei >= sc->cfg.max_edges || ej >= sc->cfg.max_edges) continue;
            if (strcmp(sc->funcs[i].file_ext, sc->funcs[j].file_ext) != 0) continue;

            float score = cbm_sem_combined_score(&sc->funcs[i], &sc->funcs[j], &sc->cfg);
            if (score < sc->cfg.threshold) continue;

            bool same_file = sc->funcs[i].file_path && sc->funcs[j].file_path &&
                             strcmp(sc->funcs[i].file_path, sc->funcs[j].file_path) == 0;
            deferred_buf_push(my_buf, sc->funcs[i].node_id, sc->funcs[j].node_id,
                              score, same_file);
            atomic_fetch_add_explicit((_Atomic int *)&sc->edge_counts[i], 1,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit((_Atomic int *)&sc->edge_counts[j], 1,
                                      memory_order_relaxed);
        }
    }
}

/* ── Parallel Phase 1b: decode minhash/profile + build per-func vectors ── */

typedef struct {
    cbm_sem_func_t *funcs;
    const cbm_gbuf_node_t **node_ptrs;
    const cbm_gbuf_t *gbuf;
    int func_count;
    _Atomic int next_idx;
} collect_ctx_t;

static void collect_worker(int worker_id, void *ctx_ptr) {
    (void)worker_id;
    collect_ctx_t *cc = ctx_ptr;
    while (1) {
        int f = atomic_fetch_add_explicit(&cc->next_idx, 64, memory_order_relaxed);
        if (f >= cc->func_count) break;
        int end = f + 64;
        if (end > cc->func_count) end = cc->func_count;
        for (int i = f; i < end; i++) {
            const cbm_gbuf_node_t *n = cc->node_ptrs[i];
            decode_minhash(n->properties_json, &cc->funcs[i]);
            decode_struct_profile(n->properties_json, cc->funcs[i].struct_profile);
            build_api_vec(cc->gbuf, n->id, &cc->funcs[i].api_vec);
            build_type_vec(n->properties_json, &cc->funcs[i].type_vec);
            build_deco_vec(n->properties_json, &cc->funcs[i].deco_vec);
        }
    }
}

/* ── Pass entry point ────────────────────────────────────────────── */

int cbm_pipeline_pass_semantic_edges(cbm_pipeline_ctx_t *ctx) {
    /* Controlled by pipeline mode (moderate/full), not env var */
    cbm_log_info("pass.start", "pass", "semantic_edges");

    cbm_gbuf_t *gbuf = ctx->gbuf;
    cbm_sem_config_t cfg = cbm_sem_get_config();

    CBM_PROF_START(t_phase1a);
    /* Phase 1a: Fast scan — collect node pointers + basic fields (SEQUENTIAL, tiny).
     * Pre-sizes the arrays so Phase 1b can write in parallel. */
    const char *labels[] = {"Function", "Method", NULL};
    cbm_sem_func_t *funcs = NULL;
    const cbm_gbuf_node_t **node_ptrs = NULL;
    int func_count = 0;
    int func_cap = 0;

    for (int li = 0; labels[li]; li++) {
        const cbm_gbuf_node_t **nodes = NULL;
        int node_count = 0;
        if (cbm_gbuf_find_by_label(gbuf, labels[li], &nodes, &node_count) != 0) {
            continue;
        }
        for (int i = 0; i < node_count; i++) {
            if (func_count >= func_cap) {
                int new_cap = func_cap < MAX_FUNCS_INIT ? MAX_FUNCS_INIT : func_cap * GROW;
                cbm_sem_func_t *grown = realloc(funcs, (size_t)new_cap * sizeof(cbm_sem_func_t));
                const cbm_gbuf_node_t **np_grown =
                    realloc(node_ptrs, (size_t)new_cap * sizeof(cbm_gbuf_node_t *));
                if (!grown || !np_grown) {
                    break;
                }
                funcs = grown;
                node_ptrs = np_grown;
                func_cap = new_cap;
            }
            memset(&funcs[func_count], 0, sizeof(cbm_sem_func_t));
            funcs[func_count].node_id = nodes[i]->id;
            funcs[func_count].file_path = nodes[i]->file_path;
            funcs[func_count].file_ext = file_ext(nodes[i]->file_path);
            node_ptrs[func_count] = nodes[i];
            func_count++;
        }
    }
    CBM_PROF_END_N("semantic_edges", "1a_scan_seq", t_phase1a, func_count);

    /* Phase 1b: Decode minhash + profile + build api/type/deco vectors (PARALLEL).
     * Must eagerly init the pretrained token map before parallel dispatch —
     * the lazy init inside cbm_sem_random_index is not thread-safe for first-use
     * races, and build_api_vec calls cbm_sem_random_index via its callees. */
    cbm_sem_ensure_ready();
    CBM_PROF_START(t_phase1b);
    if (func_count > 0) {
        int wc = cbm_default_worker_count(false);
        collect_ctx_t cc = {
            .funcs = funcs,
            .node_ptrs = node_ptrs,
            .gbuf = gbuf,
            .func_count = func_count,
        };
        atomic_init(&cc.next_idx, 0);
        cbm_parallel_for_opts_t opts = {.max_workers = wc, .force_pthreads = false};
        cbm_parallel_for(wc, collect_worker, &cc, opts);
    }
    CBM_PROF_END_N("semantic_edges", "1b_decode_build_parallel", t_phase1b, func_count);
    cbm_log_info("pass.semantic.collected", "functions", itoa_log(func_count));

    if (func_count < 2) {
        free(funcs);
        free(node_ptrs);
        cbm_log_info("pass.done", "pass", "semantic_edges", "edges", "0");
        return 0;
    }

    /* Phase 2: Tokenize all nodes (PARALLEL) */
    int worker_count = cbm_default_worker_count(false);
    char **all_tokens = malloc((size_t)func_count * sizeof(char *) * CBM_SEM_MAX_TOKENS);
    int *token_counts = calloc((size_t)func_count, sizeof(int));

    CBM_PROF_START(t_phase2);
    {
        tokenize_ctx_t tc = {
            .node_ptrs = node_ptrs, .gbuf = gbuf,
            .all_tokens = all_tokens, .token_counts = token_counts,
            .func_count = func_count,
        };
        atomic_init(&tc.next_idx, 0);
        cbm_parallel_for_opts_t opts = {.max_workers = worker_count, .force_pthreads = false};
        cbm_parallel_for(worker_count, tokenize_worker, &tc, opts);
    }
    CBM_PROF_END_N("semantic_edges", "2_tokenize_parallel", t_phase2, func_count);
    free(node_ptrs);

    /* Phase 3a: Build corpus from pre-computed tokens (batch API, partially PARALLEL).
     * Phase A: sequential token_map build. Phase B: parallel token→ID resolve
     * + doc_freq via atomic counters. Phase C: sequential reduce. */
    CBM_PROF_START(t_phase3a);
    cbm_sem_corpus_t *corpus = cbm_sem_corpus_new();
    cbm_sem_corpus_add_docs_batch(corpus, all_tokens, token_counts, func_count,
                                   CBM_SEM_MAX_TOKENS);
    CBM_PROF_END_N("semantic_edges", "3a_corpus_batch", t_phase3a, func_count);

    /* Phase 3b: Finalize corpus — IDF + co-occurrence enriched RI vectors (sequential) */
    CBM_PROF_START(t_phase3b);
    cbm_sem_corpus_finalize(corpus);
    CBM_PROF_END_N("semantic_edges", "3b_corpus_finalize_seq", t_phase3b,
                   cbm_sem_corpus_token_count(corpus));

    /* Phase 3c: Export enriched token vectors to graph buffer for query-time lookup */
    CBM_PROF_START(t_phase3c);
    {
        int tv_count = cbm_sem_corpus_token_count(corpus);
        for (int t = 0; t < tv_count; t++) {
            const cbm_sem_vec_t *vec = NULL;
            float idf = 0.0f;
            const char *tok = cbm_sem_corpus_token_at(corpus, t, &vec, &idf);
            if (tok && vec && idf > 0.01f) {
                /* Int8 quantize the enriched vector */
                uint8_t qvec[CBM_SEM_DIM];
                for (int d = 0; d < CBM_SEM_DIM; d++) {
                    float clamped = vec->v[d];
                    if (clamped > 1.0f) {
                        clamped = 1.0f;
                    }
                    if (clamped < -1.0f) {
                        clamped = -1.0f;
                    }
                    qvec[d] = (uint8_t)(int8_t)(clamped * 127.0f);
                }
                cbm_gbuf_store_token_vector(gbuf, tok, qvec, CBM_SEM_DIM, idf);
            }
        }
        cbm_log_info("pass.semantic.token_vectors", "count",
                     itoa_log(cbm_sem_corpus_token_count(corpus)));
    }
    CBM_PROF_END_N("semantic_edges", "3c_token_vec_export_seq", t_phase3c,
                   cbm_sem_corpus_token_count(corpus));

    /* Phase 4a: Build per-function TF-IDF + RI vectors + int8 quantize (PARALLEL) */
    CBM_PROF_START(t_phase4a);
    uint8_t *qvecs = malloc((size_t)func_count * CBM_SEM_DIM);
    {
        vec_build_ctx_t vc = {
            .funcs = funcs, .all_tokens = all_tokens, .token_counts = token_counts,
            .corpus = corpus, .qvecs = qvecs, .func_count = func_count,
        };
        atomic_init(&vc.next_idx, 0);
        cbm_parallel_for_opts_t opts = {.max_workers = worker_count, .force_pthreads = false};
        cbm_parallel_for(worker_count, vec_build_worker, &vc, opts);
    }
    CBM_PROF_END_N("semantic_edges", "4a_build_func_vec_parallel", t_phase4a, func_count);

    /* Phase 4b: Store pre-quantized vectors in gbuf (sequential — gbuf not thread-safe) */
    CBM_PROF_START(t_phase4b);
    for (int f = 0; f < func_count; f++) {
        cbm_gbuf_store_vector(gbuf, funcs[f].node_id, &qvecs[f * CBM_SEM_DIM], CBM_SEM_DIM);
    }
    free(qvecs);
    CBM_PROF_END_N("semantic_edges", "4b_vec_store_gbuf_seq", t_phase4b, func_count);

    cbm_log_info("pass.semantic.vectors_stored", "count", itoa_log(func_count));

    /* Phase 5a: Generate deterministic random hyperplanes */
    CBM_PROF_START(t_phase5a);
    float (*hyperplanes)[CBM_SEM_DIM] = malloc(sizeof(float[NUM_HYPERPLANES][CBM_SEM_DIM]));
    for (int h = 0; h < NUM_HYPERPLANES; h++) {
        for (int d = 0; d < CBM_SEM_DIM; d++) {
            uint64_t seed = XXH3_64bits_withSeed(&d, sizeof(d), (uint64_t)h * CBM_SEM_DIM);
            hyperplanes[h][d] = ((float)(seed & UINT32_MAX) / (float)UINT32_MAX) - 0.5f;
        }
    }
    CBM_PROF_END("semantic_edges", "5a_hyperplanes", t_phase5a);

    /* Phase 5b: Compute bit signatures (PARALLEL) */
    CBM_PROF_START(t_phase5b);
    uint64_t *signatures = calloc((size_t)func_count, sizeof(uint64_t));
    {
        sig_build_ctx_t sc = {
            .funcs = funcs, .signatures = signatures, .hyperplanes = hyperplanes,
            .func_count = func_count,
        };
        atomic_init(&sc.next_idx, 0);
        cbm_parallel_for_opts_t opts = {.max_workers = worker_count, .force_pthreads = false};
        cbm_parallel_for(worker_count, sig_build_worker, &sc, opts);
    }
    free(hyperplanes);
    CBM_PROF_END_N("semantic_edges", "5b_lsh_signatures_parallel", t_phase5b, func_count);

    /* Phase 5c: Build band-based LSH buckets (sequential — bucket realloc not thread-safe) */
    CBM_PROF_START(t_phase5c);
    typedef struct {
        int *items;
        int count;
        int cap;
    } sem_bucket_t;

    sem_bucket_t **band_buckets = calloc(SEM_LSH_BANDS, sizeof(sem_bucket_t *));
    for (int b = 0; b < SEM_LSH_BANDS; b++) {
        band_buckets[b] = calloc(SEM_BUCKET_COUNT, sizeof(sem_bucket_t));
    }

    for (int f = 0; f < func_count; f++) {
        for (int b = 0; b < SEM_LSH_BANDS; b++) {
            /* Extract SEM_LSH_ROWS bits starting at bit b*SEM_LSH_ROWS */
            int shift = b * SEM_LSH_ROWS;
            uint32_t band_val = (uint32_t)((signatures[f] >> shift) & ((1ULL << SEM_LSH_ROWS) - 1));
            /* Hash to bucket (combine band_val with band index for better distribution) */
            uint64_t bh = XXH3_64bits_withSeed(&band_val, sizeof(band_val), (uint64_t)b);
            uint32_t bucket_idx = (uint32_t)(bh & SEM_BUCKET_MASK);
            sem_bucket_t *bucket = &band_buckets[b][bucket_idx];
            if (bucket->count >= bucket->cap) {
                int nc =
                    bucket->cap < SEM_BUCKET_CAP_INIT ? SEM_BUCKET_CAP_INIT : bucket->cap * GROW;
                int *ni = realloc(bucket->items, (size_t)nc * sizeof(int));
                if (!ni) {
                    continue;
                }
                bucket->items = ni;
                bucket->cap = nc;
            }
            bucket->items[bucket->count++] = f;
        }
    }
    CBM_PROF_END_N("semantic_edges", "5c_lsh_buckets_seq", t_phase5c, func_count);

    cbm_log_info("pass.semantic.lsh_built", "functions", itoa_log(func_count), "bands",
                 itoa_log(SEM_LSH_BANDS));

    /* Phase 6a: Score candidates (PARALLEL) — edges collected in per-worker buffers */
    CBM_PROF_START(t_phase6a);
    int *edge_counts = calloc((size_t)func_count, sizeof(int));
    deferred_edge_buf_t *worker_bufs = calloc((size_t)worker_count, sizeof(deferred_edge_buf_t));
    for (int w = 0; w < worker_count; w++) {
        deferred_buf_init(&worker_bufs[w]);
    }

    {
        score_ctx_t sc = {
            .funcs = funcs, .signatures = signatures, .edge_counts = edge_counts,
            .cfg = cfg, .func_count = func_count,
            .band_buckets = (void *)band_buckets,
            .worker_bufs = worker_bufs, .max_workers = worker_count,
        };
        atomic_init(&sc.next_idx, 0);
        cbm_parallel_for_opts_t opts = {.max_workers = worker_count, .force_pthreads = false};
        cbm_parallel_for(worker_count, score_worker, &sc, opts);
    }
    CBM_PROF_END_N("semantic_edges", "6a_score_parallel", t_phase6a, func_count);

    /* Phase 6b: Merge deferred edges into gbuf (sequential) */
    CBM_PROF_START(t_phase6b);
    int total_edges = 0;
    for (int w = 0; w < worker_count; w++) {
        for (int e = 0; e < worker_bufs[w].count; e++) {
            deferred_edge_t *de = &worker_bufs[w].edges[e];
            char props[PROPS_BUF];
            snprintf(props, sizeof(props), "{\"score\":%.3f,\"same_file\":%s}", de->score,
                     de->same_file ? "true" : "false");
            cbm_gbuf_insert_edge(gbuf, de->source_id, de->target_id,
                                 "SEMANTICALLY_RELATED", props);
            total_edges++;
        }
        deferred_buf_free(&worker_bufs[w]);
    }
    free(worker_bufs);
    CBM_PROF_END_N("semantic_edges", "6b_edge_merge_seq", t_phase6b, total_edges);

    /* Phase 7: Cleanup */
    CBM_PROF_START(t_phase7);
    /* Free LSH buckets */
    for (int b = 0; b < SEM_LSH_BANDS; b++) {
        for (int h = 0; h < SEM_BUCKET_COUNT; h++) {
            free(band_buckets[b][h].items);
        }
        free(band_buckets[b]);
    }
    free(band_buckets);
    free(signatures);

    cbm_log_info("pass.done", "pass", "semantic_edges", "edges", itoa_log(total_edges));

    free(edge_counts);
    for (int f = 0; f < func_count; f++) {
        free(funcs[f].tfidf_indices);
        free(funcs[f].tfidf_weights);
        int tc = token_counts[f];
        for (int t = 0; t < tc; t++) {
            free(all_tokens[f * CBM_SEM_MAX_TOKENS + t]);
        }
    }
    free(all_tokens);
    free(token_counts);
    free(funcs);
    cbm_sem_corpus_free(corpus);
    CBM_PROF_END("semantic_edges", "7_cleanup", t_phase7);

    return 0;
}
