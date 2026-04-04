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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

enum { PROPS_BUF = 512, MAX_FUNCS_INIT = 4096, GROW = 2, MAX_CALLEES = 64 };

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

static int tokenize_node(const cbm_gbuf_node_t *n, char **tokens, int max_tokens) {
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

/* ── Pass entry point ────────────────────────────────────────────── */

int cbm_pipeline_pass_semantic_edges(cbm_pipeline_ctx_t *ctx) {
    /* Controlled by pipeline mode (moderate/full), not env var */
    cbm_log_info("pass.start", "pass", "semantic_edges");

    cbm_gbuf_t *gbuf = ctx->gbuf;
    cbm_sem_config_t cfg = cbm_sem_get_config();

    /* Phase 1: Collect all Function/Method nodes */
    const char *labels[] = {"Function", "Method", NULL};
    cbm_sem_func_t *funcs = NULL;
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
                if (!grown) {
                    break;
                }
                funcs = grown;
                func_cap = new_cap;
            }
            memset(&funcs[func_count], 0, sizeof(cbm_sem_func_t));
            funcs[func_count].node_id = nodes[i]->id;
            funcs[func_count].file_path = nodes[i]->file_path;
            funcs[func_count].file_ext = file_ext(nodes[i]->file_path);

            /* Decode MinHash and AST profile from properties */
            decode_minhash(nodes[i]->properties_json, &funcs[func_count]);
            decode_struct_profile(nodes[i]->properties_json, funcs[func_count].struct_profile);

            /* Build API, Type, Decorator vectors */
            build_api_vec(gbuf, nodes[i]->id, &funcs[func_count].api_vec);
            build_type_vec(nodes[i]->properties_json, &funcs[func_count].type_vec);
            build_deco_vec(nodes[i]->properties_json, &funcs[func_count].deco_vec);

            func_count++;
        }
    }

    cbm_log_info("pass.semantic.collected", "functions", itoa_log(func_count));

    if (func_count < 2) {
        free(funcs);
        cbm_log_info("pass.done", "pass", "semantic_edges", "edges", "0");
        return 0;
    }

    /* Phase 2+3: Tokenize all nodes + build corpus for IDF + RI */
    cbm_sem_corpus_t *corpus = cbm_sem_corpus_new();
    char **all_tokens = malloc((size_t)func_count * sizeof(char *) * CBM_SEM_MAX_TOKENS);
    int *token_counts = calloc((size_t)func_count, sizeof(int));

    /* Re-iterate nodes for tokenization (indices match funcs[]) */
    int fi = 0;
    for (int li = 0; labels[li]; li++) {
        const cbm_gbuf_node_t **nodes = NULL;
        int node_count = 0;
        if (cbm_gbuf_find_by_label(gbuf, labels[li], &nodes, &node_count) != 0) {
            continue;
        }
        for (int i = 0; i < node_count && fi < func_count; i++, fi++) {
            char *tokens[CBM_SEM_MAX_TOKENS];
            int tc = tokenize_node(nodes[i], tokens, CBM_SEM_MAX_TOKENS);
            token_counts[fi] = tc;

            /* Store token pointers for later TF-IDF/RI vector construction */
            for (int t = 0; t < tc; t++) {
                all_tokens[fi * CBM_SEM_MAX_TOKENS + t] = tokens[t];
            }

            /* Register in corpus */
            cbm_sem_corpus_add_doc(corpus, (const char **)tokens, tc);
        }
    }

    /* Finalize corpus: compute IDF + co-occurrence enriched RI vectors */
    cbm_sem_corpus_finalize(corpus);

    /* Phase 4: Build per-function TF-IDF + RI vectors */
    for (int f = 0; f < func_count; f++) {
        int tc = token_counts[f];
        char **tokens = &all_tokens[f * CBM_SEM_MAX_TOKENS];

        /* TF-IDF: build sparse vector (sorted by token index for merge) */
        /* Simple approach: use token indices from corpus */
        int *indices = malloc((size_t)tc * sizeof(int));
        float *weights = malloc((size_t)tc * sizeof(float));
        int tfidf_len = 0;

        for (int t = 0; t < tc; t++) {
            float idf = cbm_sem_corpus_idf(corpus, tokens[t]);
            if (idf > 0.0f) {
                /* Simple TF = 1 (presence, not frequency — tokens from metadata are short) */
                indices[tfidf_len] = t; /* use sequential index — sorted by construction */
                weights[tfidf_len] = idf;
                tfidf_len++;
            }
        }
        funcs[f].tfidf_indices = indices;
        funcs[f].tfidf_weights = weights;
        funcs[f].tfidf_len = tfidf_len;

        /* RI vector: sum of enriched token vectors */
        memset(&funcs[f].ri_vec, 0, sizeof(cbm_sem_vec_t));
        for (int t = 0; t < tc; t++) {
            const cbm_sem_vec_t *ri = cbm_sem_corpus_ri_vec(corpus, tokens[t]);
            if (ri) {
                float idf = cbm_sem_corpus_idf(corpus, tokens[t]);
                cbm_sem_vec_add_scaled(&funcs[f].ri_vec, ri, idf);
            }
        }
        cbm_sem_normalize(&funcs[f].ri_vec);

        /* Int8-quantize and store in graph buffer for vector search at query time */
        uint8_t qvec[CBM_SEM_DIM];
        for (int d = 0; d < CBM_SEM_DIM; d++) {
            float clamped = funcs[f].ri_vec.v[d];
            if (clamped > 1.0f) {
                clamped = 1.0f;
            }
            if (clamped < -1.0f) {
                clamped = -1.0f;
            }
            qvec[d] = (uint8_t)(int8_t)(clamped * 127.0f);
        }
        cbm_gbuf_store_vector(gbuf, funcs[f].node_id, qvec, CBM_SEM_DIM);
    }

    cbm_log_info("pass.semantic.vectors_stored", "count", itoa_log(func_count));

    /* Phase 5: Build cosine-LSH index using random hyperplanes on RI vectors.
     * This gives O(n) candidate generation instead of O(n²) brute-force.
     *
     * Method: Generate NUM_HYPERPLANES random d-dim vectors. For each function,
     * compute sign(dot(ri_vec, hyperplane)) for each → a bit signature.
     * Group bits into LSH_BANDS bands of LSH_ROWS bits. Hash each band → bucket.
     * Functions sharing any bucket are candidates. */
    enum {
        NUM_HYPERPLANES = 64,
        SEM_LSH_BANDS = 16,
        SEM_LSH_ROWS = 4,
        SEM_BUCKET_COUNT = 65536,
        SEM_BUCKET_MASK = 65535,
        SEM_BUCKET_CAP_INIT = 16,
        SEM_MAX_CANDIDATES = 200,
    };

    /* Generate deterministic random hyperplanes */
    float hyperplanes[NUM_HYPERPLANES][CBM_SEM_DIM];
    for (int h = 0; h < NUM_HYPERPLANES; h++) {
        for (int d = 0; d < CBM_SEM_DIM; d++) {
            uint64_t seed = XXH3_64bits_withSeed(&d, sizeof(d), (uint64_t)h * CBM_SEM_DIM);
            hyperplanes[h][d] = ((float)(seed & UINT32_MAX) / (float)UINT32_MAX) - 0.5f;
        }
    }

    /* Compute bit signatures for each function */
    uint64_t *signatures = calloc((size_t)func_count, sizeof(uint64_t));
    for (int f = 0; f < func_count; f++) {
        uint64_t sig = 0;
        for (int h = 0; h < NUM_HYPERPLANES; h++) {
            float dot = 0.0f;
            for (int d = 0; d < CBM_SEM_DIM; d++) {
                dot += funcs[f].ri_vec.v[d] * hyperplanes[h][d];
            }
            if (dot > 0.0f) {
                sig |= (1ULL << h);
            }
        }
        signatures[f] = sig;
    }

    /* Build band-based LSH buckets.
     * Each band = SEM_LSH_ROWS consecutive bits → hash to a bucket. */
    typedef struct {
        int *items;
        int count;
        int cap;
    } sem_bucket_t;

    /* Allocate bands × buckets (sparse — only allocate used buckets) */
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

    cbm_log_info("pass.semantic.lsh_built", "functions", itoa_log(func_count), "bands",
                 itoa_log(SEM_LSH_BANDS));

    /* Phase 6: Query LSH and score candidates.
     * For each function, collect unique candidates from all bands, score, emit edges. */
    int total_edges = 0;
    int *edge_counts = calloc((size_t)func_count, sizeof(int));

    /* Reusable per-query seen set (open addressing on func index) */
    enum { SEEN_CAP = 8192, SEEN_MASK = 8191 };

    for (int i = 0; i < func_count; i++) {
        if (edge_counts[i] >= cfg.max_edges) {
            continue;
        }

        /* Collect unique candidates from all bands */
        int seen[SEEN_CAP];
        memset(seen, -1, sizeof(seen));
        int candidates[SEM_MAX_CANDIDATES];
        int cand_count = 0;

        for (int b = 0; b < SEM_LSH_BANDS && cand_count < SEM_MAX_CANDIDATES; b++) {
            int shift = b * SEM_LSH_ROWS;
            uint32_t band_val = (uint32_t)((signatures[i] >> shift) & ((1ULL << SEM_LSH_ROWS) - 1));
            uint64_t bh = XXH3_64bits_withSeed(&band_val, sizeof(band_val), (uint64_t)b);
            uint32_t bucket_idx = (uint32_t)(bh & SEM_BUCKET_MASK);
            sem_bucket_t *bucket = &band_buckets[b][bucket_idx];

            /* Skip oversized buckets (noise) */
            if (bucket->count > SEM_MAX_CANDIDATES) {
                continue;
            }

            for (int k = 0; k < bucket->count && cand_count < SEM_MAX_CANDIDATES; k++) {
                int j = bucket->items[k];
                if (j <= i) {
                    continue; /* only emit A→B where A < B */
                }
                /* Dedup via open-addressing seen set */
                uint32_t slot = (uint32_t)j & SEEN_MASK;
                bool dup = false;
                for (int p = 0; p < SEEN_CAP; p++) {
                    uint32_t idx = (slot + (uint32_t)p) & SEEN_MASK;
                    if (seen[idx] == -1) {
                        seen[idx] = j;
                        break;
                    }
                    if (seen[idx] == j) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    continue;
                }
                candidates[cand_count++] = j;
            }
        }

        /* Score candidates */
        for (int c = 0; c < cand_count; c++) {
            int j = candidates[c];
            if (edge_counts[i] >= cfg.max_edges || edge_counts[j] >= cfg.max_edges) {
                continue;
            }
            if (strcmp(funcs[i].file_ext, funcs[j].file_ext) != 0) {
                continue;
            }
            float score = cbm_sem_combined_score(&funcs[i], &funcs[j], &cfg);
            if (score < cfg.threshold) {
                continue;
            }
            bool same_file = funcs[i].file_path && funcs[j].file_path &&
                             strcmp(funcs[i].file_path, funcs[j].file_path) == 0;
            char props[PROPS_BUF];
            snprintf(props, sizeof(props), "{\"score\":%.3f,\"same_file\":%s}", score,
                     same_file ? "true" : "false");
            cbm_gbuf_insert_edge(gbuf, funcs[i].node_id, funcs[j].node_id, "SEMANTICALLY_RELATED",
                                 props);
            edge_counts[i]++;
            edge_counts[j]++;
            total_edges++;
        }
    }

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

    /* Cleanup */
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

    return 0;
}
