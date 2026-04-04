/*
 * semantic.c — Algorithmic code embeddings: TF-IDF, Random Indexing,
 * API/Type/Decorator signatures, combined scoring, graph diffusion.
 *
 * All signals computed from graph buffer metadata — no source file reads.
 * Uses xxHash for deterministic random vectors. Pure C, zero dependencies.
 */
#include "semantic/semantic.h"
#include "foundation/constants.h"
#include "foundation/hash_table.h"
#include "foundation/log.h"
#include "simhash/minhash.h"

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ───────────────────────────────────────────────────── */

enum {
    TOKEN_BUF_LEN = 128,
    CORPUS_INIT_CAP = 4096,
    DOC_TOKENS_INIT = 64,
    RI_SEED_BASE = 0x52494E44, /* "RIND" */
};

/* ── Configuration ───────────────────────────────────────────────── */

cbm_sem_config_t cbm_sem_get_config(void) {
    cbm_sem_config_t cfg = {
        .w_tfidf = 0.20f,
        .w_ri = 0.25f,
        .w_minhash = 0.10f,
        .w_api = 0.15f,
        .w_type = 0.10f,
        .w_decorator = 0.05f,
        .w_struct_profile = 0.10f,
        .w_dataflow = 0.05f,
        .threshold = (float)CBM_SEM_EDGE_THRESHOLD,
        .max_edges = CBM_SEM_MAX_EDGES,
    };
    const char *thresh = getenv("CBM_SEMANTIC_THRESHOLD");
    if (thresh) {
        float t = (float)atof(thresh);
        if (t > 0.0f && t <= 1.0f) {
            cfg.threshold = t;
        }
    }
    return cfg;
}

bool cbm_sem_is_enabled(void) {
    const char *val = getenv("CBM_SEMANTIC_ENABLED");
    return val && val[0] == '1';
}

/* ── Token extraction ────────────────────────────────────────────── */

int cbm_sem_tokenize(const char *name, char **out, int max_out) {
    if (!name || !out || max_out <= 0) {
        return 0;
    }
    int count = 0;
    char buf[TOKEN_BUF_LEN];
    int blen = 0;

    for (int i = 0; name[i] && count < max_out; i++) {
        char c = name[i];
        /* Split on: dots, slashes, underscores, hyphens, camelCase transitions */
        bool is_split = (c == '.' || c == '/' || c == '_' || c == '-' || c == ' ' || c == '(' ||
                         c == ')' || c == ',' || c == ':');
        bool is_camel = (i > 0 && c >= 'A' && c <= 'Z' && name[i - SKIP_ONE] >= 'a' &&
                         name[i - SKIP_ONE] <= 'z');

        if (is_split || is_camel) {
            if (blen > 0) {
                buf[blen] = '\0';
                out[count++] = strdup(buf);
                blen = 0;
            }
            if (is_split) {
                continue;
            }
        }
        if (blen < TOKEN_BUF_LEN - SKIP_ONE && isalnum((unsigned char)c)) {
            buf[blen++] = (char)tolower((unsigned char)c);
        }
    }
    if (blen > 0 && count < max_out) {
        buf[blen] = '\0';
        out[count++] = strdup(buf);
    }
    return count;
}

/* ── Dense vector operations ─────────────────────────────────────── */

float cbm_sem_cosine(const cbm_sem_vec_t *a, const cbm_sem_vec_t *b) {
    if (!a || !b) {
        return 0.0f;
    }
    float dot = 0.0f;
    float mag_a = 0.0f;
    float mag_b = 0.0f;
    for (int i = 0; i < CBM_SEM_DIM; i++) {
        dot += a->v[i] * b->v[i];
        mag_a += a->v[i] * a->v[i];
        mag_b += b->v[i] * b->v[i];
    }
    float denom = sqrtf(mag_a) * sqrtf(mag_b);
    if (denom < 1e-10f) {
        return 0.0f;
    }
    return dot / denom;
}

void cbm_sem_random_index(const char *token, cbm_sem_vec_t *out) {
    memset(out, 0, sizeof(*out));
    if (!token) {
        return;
    }
    uint64_t seed = XXH3_64bits(token, strlen(token));
    /* Generate SPARSE_NNZE non-zero entries at deterministic positions */
    for (int i = 0; i < CBM_SEM_SPARSE_NNZE; i++) {
        uint64_t h = XXH3_64bits_withSeed(&i, sizeof(i), seed + RI_SEED_BASE);
        int pos = (int)(h % CBM_SEM_DIM);
        float sign = (h & SKIP_ONE) ? 1.0f : -1.0f;
        out->v[pos] += sign;
    }
}

void cbm_sem_normalize(cbm_sem_vec_t *v) {
    if (!v) {
        return;
    }
    float mag = 0.0f;
    for (int i = 0; i < CBM_SEM_DIM; i++) {
        mag += v->v[i] * v->v[i];
    }
    mag = sqrtf(mag);
    if (mag < 1e-10f) {
        return;
    }
    float inv = 1.0f / mag;
    for (int i = 0; i < CBM_SEM_DIM; i++) {
        v->v[i] *= inv;
    }
}

void cbm_sem_vec_add_scaled(cbm_sem_vec_t *dst, const cbm_sem_vec_t *src, float scale) {
    if (!dst || !src) {
        return;
    }
    for (int i = 0; i < CBM_SEM_DIM; i++) {
        dst->v[i] += scale * src->v[i];
    }
}

/* ── Corpus (IDF + Random Indexing enrichment) ───────────────────── */

typedef struct {
    char *token;
    int doc_freq;
    cbm_sem_vec_t enriched_vec; /* context-enriched via co-occurrence */
} corpus_entry_t;

struct cbm_sem_corpus {
    CBMHashTable *token_map; /* token → index into entries[] */
    corpus_entry_t *entries;
    int entry_count;
    int entry_cap;
    int doc_count;
    bool finalized;

    /* Per-document token lists for co-occurrence pass */
    int **doc_token_ids;
    int *doc_token_counts;
    int doc_cap;
};

static int corpus_get_or_add(cbm_sem_corpus_t *c, const char *token) {
    char idx_buf[CBM_SZ_16];
    const char *existing = cbm_ht_get(c->token_map, token);
    if (existing) {
        return atoi(existing);
    }
    if (c->entry_count >= c->entry_cap) {
        int new_cap = c->entry_cap < CORPUS_INIT_CAP ? CORPUS_INIT_CAP : c->entry_cap * 2;
        corpus_entry_t *grown = realloc(c->entries, (size_t)new_cap * sizeof(corpus_entry_t));
        if (!grown) {
            return -1;
        }
        c->entries = grown;
        c->entry_cap = new_cap;
    }
    int idx = c->entry_count++;
    c->entries[idx].token = strdup(token);
    c->entries[idx].doc_freq = 0;
    memset(&c->entries[idx].enriched_vec, 0, sizeof(cbm_sem_vec_t));
    snprintf(idx_buf, sizeof(idx_buf), "%d", idx);
    cbm_ht_set(c->token_map, strdup(token), strdup(idx_buf));
    return idx;
}

cbm_sem_corpus_t *cbm_sem_corpus_new(void) {
    cbm_sem_corpus_t *c = calloc(SKIP_ONE, sizeof(cbm_sem_corpus_t));
    if (c) {
        c->token_map = cbm_ht_create(CORPUS_INIT_CAP);
    }
    return c;
}

void cbm_sem_corpus_add_doc(cbm_sem_corpus_t *corpus, const char **tokens, int count) {
    if (!corpus || !tokens || count <= 0) {
        return;
    }
    /* Track document for co-occurrence pass */
    if (corpus->doc_count >= corpus->doc_cap) {
        int new_cap = corpus->doc_cap < DOC_TOKENS_INIT ? DOC_TOKENS_INIT : corpus->doc_cap * 2;
        corpus->doc_token_ids = realloc(corpus->doc_token_ids, (size_t)new_cap * sizeof(int *));
        corpus->doc_token_counts = realloc(corpus->doc_token_counts, (size_t)new_cap * sizeof(int));
        corpus->doc_cap = new_cap;
    }
    int doc_idx = corpus->doc_count++;
    corpus->doc_token_ids[doc_idx] = malloc((size_t)count * sizeof(int));
    corpus->doc_token_counts[doc_idx] = count;

    /* Per-doc unique set for IDF */
    int *seen = calloc((size_t)corpus->entry_cap + (size_t)count + CORPUS_INIT_CAP, sizeof(int));
    int seen_count = 0;

    for (int i = 0; i < count; i++) {
        int tid = corpus_get_or_add(corpus, tokens[i]);
        corpus->doc_token_ids[doc_idx][i] = tid;
        if (tid < 0) {
            continue;
        }
        /* Check uniqueness for IDF (simple linear scan — tokens per doc is small) */
        bool is_new = true;
        for (int j = 0; j < seen_count; j++) {
            if (seen[j] == tid) {
                is_new = false;
                break;
            }
        }
        if (is_new) {
            seen[seen_count++] = tid;
            corpus->entries[tid].doc_freq++;
        }
    }
    free(seen);
}

void cbm_sem_corpus_finalize(cbm_sem_corpus_t *corpus) {
    if (!corpus || corpus->finalized) {
        return;
    }

    /* Initialize base random index vectors for each token */
    for (int i = 0; i < corpus->entry_count; i++) {
        cbm_sem_random_index(corpus->entries[i].token, &corpus->entries[i].enriched_vec);
    }

    /* Co-occurrence pass: for each document, accumulate neighbor vectors */
    for (int d = 0; d < corpus->doc_count; d++) {
        int *ids = corpus->doc_token_ids[d];
        int len = corpus->doc_token_counts[d];
        for (int i = 0; i < len; i++) {
            if (ids[i] < 0) {
                continue;
            }
            cbm_sem_vec_t *target = &corpus->entries[ids[i]].enriched_vec;
            /* Add neighbor vectors within window */
            for (int w = -CBM_SEM_WINDOW; w <= CBM_SEM_WINDOW; w++) {
                if (w == 0) {
                    continue;
                }
                int j = i + w;
                if (j < 0 || j >= len || ids[j] < 0) {
                    continue;
                }
                float weight = 1.0f / (float)abs(w); /* closer = more influence */
                cbm_sem_vec_t neighbor_ri;
                cbm_sem_random_index(corpus->entries[ids[j]].token, &neighbor_ri);
                cbm_sem_vec_add_scaled(target, &neighbor_ri, weight);
            }
        }
    }

    /* Normalize all enriched vectors */
    for (int i = 0; i < corpus->entry_count; i++) {
        cbm_sem_normalize(&corpus->entries[i].enriched_vec);
    }

    corpus->finalized = true;
}

float cbm_sem_corpus_idf(const cbm_sem_corpus_t *corpus, const char *token) {
    if (!corpus || !token || corpus->doc_count == 0) {
        return 0.0f;
    }
    const char *idx_str = cbm_ht_get(corpus->token_map, token);
    if (!idx_str) {
        return 0.0f;
    }
    int idx = atoi(idx_str);
    if (idx < 0 || idx >= corpus->entry_count) {
        return 0.0f;
    }
    int df = corpus->entries[idx].doc_freq;
    if (df <= 0) {
        return 0.0f;
    }
    return logf((float)corpus->doc_count / (float)df);
}

const cbm_sem_vec_t *cbm_sem_corpus_ri_vec(const cbm_sem_corpus_t *corpus, const char *token) {
    if (!corpus || !token) {
        return NULL;
    }
    const char *idx_str = cbm_ht_get(corpus->token_map, token);
    if (!idx_str) {
        return NULL;
    }
    int idx = atoi(idx_str);
    if (idx < 0 || idx >= corpus->entry_count) {
        return NULL;
    }
    return &corpus->entries[idx].enriched_vec;
}

int cbm_sem_corpus_doc_count(const cbm_sem_corpus_t *corpus) {
    return corpus ? corpus->doc_count : 0;
}

void cbm_sem_corpus_free(cbm_sem_corpus_t *corpus) {
    if (!corpus) {
        return;
    }
    for (int i = 0; i < corpus->entry_count; i++) {
        free(corpus->entries[i].token);
    }
    free(corpus->entries);
    for (int d = 0; d < corpus->doc_count; d++) {
        free(corpus->doc_token_ids[d]);
    }
    free(corpus->doc_token_ids);
    free(corpus->doc_token_counts);
    if (corpus->token_map) {
        cbm_ht_free(corpus->token_map);
    }
    free(corpus);
}

/* ── Combined scoring ────────────────────────────────────────────── */

float cbm_sem_proximity(const char *path_a, const char *path_b) {
    if (!path_a || !path_b) {
        return 1.0f;
    }
    /* Count shared directory components */
    int shared = 0;
    int total_a = 0;
    int total_b = 0;
    const char *a = path_a;
    const char *b = path_b;
    while (*a && *b && *a == *b) {
        if (*a == '/') {
            shared++;
        }
        a++;
        b++;
    }
    for (const char *p = path_a; *p; p++) {
        if (*p == '/') {
            total_a++;
        }
    }
    for (const char *p = path_b; *p; p++) {
        if (*p == '/') {
            total_b++;
        }
    }
    int max_total = total_a > total_b ? total_a : total_b;
    if (max_total == 0) {
        return 1.0f;
    }
    float ratio = (float)shared / (float)max_total;
    /* Same file = 1.10, same dir = 1.05, distant = 1.00 */
    return 1.0f + ratio * 0.10f;
}

/* Cosine similarity for small float arrays (AST profile). */
static float small_cosine(const float *a, const float *b, int dims) {
    float dot = 0.0f;
    float ma = 0.0f;
    float mb = 0.0f;
    for (int i = 0; i < dims; i++) {
        dot += a[i] * b[i];
        ma += a[i] * a[i];
        mb += b[i] * b[i];
    }
    float denom = sqrtf(ma) * sqrtf(mb);
    return denom < 1e-10f ? 0.0f : dot / denom;
}

float cbm_sem_combined_score(const cbm_sem_func_t *a, const cbm_sem_func_t *b,
                             const cbm_sem_config_t *cfg) {
    if (!a || !b || !cfg) {
        return 0.0f;
    }
    float score = 0.0f;

    /* Signal 1: TF-IDF sparse cosine */
    if (a->tfidf_len > 0 && b->tfidf_len > 0) {
        float dot = 0.0f;
        float ma = 0.0f;
        float mb = 0.0f;
        /* Sparse dot product: O(a_len + b_len) via merge */
        int ia = 0;
        int ib = 0;
        while (ia < a->tfidf_len && ib < b->tfidf_len) {
            if (a->tfidf_indices[ia] == b->tfidf_indices[ib]) {
                dot += a->tfidf_weights[ia] * b->tfidf_weights[ib];
                ia++;
                ib++;
            } else if (a->tfidf_indices[ia] < b->tfidf_indices[ib]) {
                ia++;
            } else {
                ib++;
            }
        }
        for (int i = 0; i < a->tfidf_len; i++) {
            ma += a->tfidf_weights[i] * a->tfidf_weights[i];
        }
        for (int i = 0; i < b->tfidf_len; i++) {
            mb += b->tfidf_weights[i] * b->tfidf_weights[i];
        }
        float denom = sqrtf(ma) * sqrtf(mb);
        if (denom > 1e-10f) {
            score += cfg->w_tfidf * (dot / denom);
        }
    }

    /* Signal 2: Random Indexing */
    score += cfg->w_ri * cbm_sem_cosine(&a->ri_vec, &b->ri_vec);

    /* Signal 3: MinHash Jaccard */
    if (a->has_minhash && b->has_minhash) {
        double j = cbm_minhash_jaccard((const cbm_minhash_t *)a->minhash,
                                       (const cbm_minhash_t *)b->minhash);
        score += cfg->w_minhash * (float)j;
    }

    /* Signal 4: API Signatures */
    score += cfg->w_api * cbm_sem_cosine(&a->api_vec, &b->api_vec);

    /* Signal 5: Type Signatures */
    score += cfg->w_type * cbm_sem_cosine(&a->type_vec, &b->type_vec);

    /* Signal 7: Decorator Pattern */
    score += cfg->w_decorator * cbm_sem_cosine(&a->deco_vec, &b->deco_vec);

    /* Signal 8+9+11: Structural profile + data flow + Halstead */
    enum { SP_DIMS = 25 };
    float sp_score = small_cosine(a->struct_profile, b->struct_profile, SP_DIMS);
    score += cfg->w_struct_profile * sp_score;

    /* Signal 6: Module proximity (multiplier, not additive) */
    score *= cbm_sem_proximity(a->file_path, b->file_path);

    return score;
}

/* ── Graph diffusion ─────────────────────────────────────────────── */

void cbm_sem_diffuse(cbm_sem_vec_t *combined, const cbm_sem_vec_t *neighbors, int neighbor_count,
                     float alpha) {
    if (!combined || !neighbors || neighbor_count <= 0) {
        return;
    }
    /* Blend: combined = (1-α) × combined + α × mean(neighbors) */
    cbm_sem_vec_t mean;
    memset(&mean, 0, sizeof(mean));
    for (int n = 0; n < neighbor_count; n++) {
        for (int i = 0; i < CBM_SEM_DIM; i++) {
            mean.v[i] += neighbors[n].v[i];
        }
    }
    float inv_n = 1.0f / (float)neighbor_count;
    float one_minus_alpha = 1.0f - alpha;
    for (int i = 0; i < CBM_SEM_DIM; i++) {
        combined->v[i] = (one_minus_alpha * combined->v[i]) + (alpha * mean.v[i] * inv_n);
    }
    cbm_sem_normalize(combined);
}
