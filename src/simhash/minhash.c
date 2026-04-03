/*
 * minhash.c — MinHash fingerprinting + LSH for near-clone detection.
 *
 * Computes K=64 MinHash signatures from normalised AST node-type
 * trigrams.  Uses xxHash with distinct seeds for each permutation.
 * Pure functions — thread-safe, no shared state.
 *
 * LSH index uses b=32 bands × r=2 rows for candidate generation.
 */
#include "simhash/minhash.h"
#include "foundation/constants.h"
#include "foundation/log.h"
/* Inline all xxHash functions — avoids separate compilation unit. */
#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"
#include "tree_sitter/api.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ── AST node type normalisation ─────────────────────────────────── */

/* Maximum trigram string length: 3 node types × max 40 chars each + separators */
enum { TRIGRAM_BUF_LEN = 160 };

/* Maximum AST body nodes to walk (stack depth). */
enum { AST_WALK_CAP = 2048 };

/* Hex encoding constants */
enum { HEX_CHARS_PER_U32 = 8, HEX_BASE = 16 };

/* Trigram window and minimum threshold */
enum { TRIGRAM_WINDOW = 2, MIN_TRIGRAM_COUNT = 3 };

/* Hash truncation mask (uint64 → uint32) */
enum { U32_MASK = 0xFFFFFFFF };

/* Dynamic array growth constants */
enum { BUCKET_INIT_CAP = 8, GROW_FACTOR = 2, ENTRY_INIT_CAP = 64, RESULT_INIT_CAP = 64 };

/* Maximum normalised tokens per function body. */
enum { MAX_TOKENS = 4096 };

/* Check if a node type is an identifier-like leaf. */
static bool is_identifier_type(const char *kind) {
    return strcmp(kind, "identifier") == 0 || strcmp(kind, "field_identifier") == 0 ||
           strcmp(kind, "property_identifier") == 0 || strcmp(kind, "type_identifier") == 0 ||
           strcmp(kind, "shorthand_property_identifier") == 0 ||
           strcmp(kind, "shorthand_field_identifier") == 0 || strcmp(kind, "variable_name") == 0 ||
           strcmp(kind, "name") == 0;
}

/* Check if a node type is a string literal. */
static bool is_string_type(const char *kind) {
    return strcmp(kind, "string") == 0 || strcmp(kind, "string_literal") == 0 ||
           strcmp(kind, "interpreted_string_literal") == 0 ||
           strcmp(kind, "raw_string_literal") == 0 || strcmp(kind, "template_string") == 0 ||
           strcmp(kind, "string_content") == 0 || strcmp(kind, "escape_sequence") == 0;
}

/* Check if a node type is a number literal. */
static bool is_number_type(const char *kind) {
    return strcmp(kind, "number") == 0 || strcmp(kind, "integer") == 0 ||
           strcmp(kind, "float") == 0 || strcmp(kind, "integer_literal") == 0 ||
           strcmp(kind, "float_literal") == 0 || strcmp(kind, "int_literal") == 0 ||
           strcmp(kind, "number_literal") == 0;
}

/* Check if a node type is a type annotation. */
static bool is_type_annotation(const char *kind) {
    return strcmp(kind, "type_identifier") == 0 || strcmp(kind, "predefined_type") == 0 ||
           strcmp(kind, "primitive_type") == 0 || strcmp(kind, "builtin_type") == 0 ||
           strcmp(kind, "type_annotation") == 0 || strcmp(kind, "simple_type") == 0;
}

/* Normalise a node type string.  Returns a short canonical string
 * or the original kind if no normalisation applies. */
static const char *normalise_node_type(const char *kind) {
    if (is_identifier_type(kind)) {
        return "I";
    }
    if (is_string_type(kind)) {
        return "S";
    }
    if (is_number_type(kind)) {
        return "N";
    }
    if (is_type_annotation(kind)) {
        return "T";
    }
    return kind;
}

/* ── MinHash computation ─────────────────────────────────────────── */

/* Phase 1: Walk AST iteratively and collect normalised token types. */
static int collect_ast_tokens(TSNode root, const char **tokens, int max_tokens) {
    int token_count = 0;
    TSNode stack[AST_WALK_CAP];
    int top = 0;
    stack[top++] = root;

    while (top > 0 && token_count < max_tokens) {
        TSNode node = stack[--top];
        uint32_t child_count = ts_node_child_count(node);
        const char *kind = ts_node_type(node);

        if (child_count == 0) {
            if (kind[0] != '\0') {
                tokens[token_count++] = normalise_node_type(kind);
            }
        } else {
            if (kind[0] != '\0' && ts_node_is_named(node)) {
                tokens[token_count++] = normalise_node_type(kind);
            }
            for (int i = (int)child_count - SKIP_ONE; i >= 0 && top < AST_WALK_CAP; i--) {
                stack[top++] = ts_node_child(node, (uint32_t)i);
            }
        }
    }
    return token_count;
}

/* Phase 2: Hash trigrams from token sequence into MinHash signature. */
static int hash_trigrams(const char **tokens, int token_count, cbm_minhash_t *out) {
    for (int k = 0; k < CBM_MINHASH_K; k++) {
        out->values[k] = UINT32_MAX;
    }

    char trigram_buf[TRIGRAM_BUF_LEN];
    int trigram_count = 0;

    for (int i = 0; i + TRIGRAM_WINDOW < token_count; i++) {
        int len = snprintf(trigram_buf, sizeof(trigram_buf), "%s|%s|%s", tokens[i], tokens[i + 1],
                           tokens[i + 2]);
        if (len <= 0 || (size_t)len >= sizeof(trigram_buf)) {
            continue;
        }
        trigram_count++;
        for (int k = 0; k < CBM_MINHASH_K; k++) {
            uint64_t h = XXH3_64bits_withSeed(trigram_buf, (size_t)len, (uint64_t)k);
            uint32_t h32 = (uint32_t)(h & U32_MASK);
            if (h32 < out->values[k]) {
                out->values[k] = h32;
            }
        }
    }
    return trigram_count;
}

bool cbm_minhash_compute(TSNode func_body, const char *source, int language, cbm_minhash_t *out) {
    (void)source;
    (void)language;

    if (ts_node_is_null(func_body)) {
        return false;
    }

    const char *tokens[MAX_TOKENS];
    int token_count = collect_ast_tokens(func_body, tokens, MAX_TOKENS);
    if (token_count < CBM_MINHASH_MIN_NODES) {
        return false;
    }

    int trigram_count = hash_trigrams(tokens, token_count, out);
    return trigram_count >= MIN_TRIGRAM_COUNT;
}

/* ── Jaccard similarity ──────────────────────────────────────────── */

double cbm_minhash_jaccard(const cbm_minhash_t *a, const cbm_minhash_t *b) {
    if (!a || !b) {
        return 0.0;
    }
    int matching = 0;
    for (int i = 0; i < CBM_MINHASH_K; i++) {
        if (a->values[i] == b->values[i]) {
            matching++;
        }
    }
    return (double)matching / (double)CBM_MINHASH_K;
}

/* ── Hex encoding/decoding ───────────────────────────────────────── */

void cbm_minhash_to_hex(const cbm_minhash_t *fp, char *buf, int bufsize) {
    if (!fp || !buf || bufsize < CBM_MINHASH_HEX_BUF) {
        if (buf && bufsize > 0) {
            buf[0] = '\0';
        }
        return;
    }
    int pos = 0;
    for (int i = 0; i < CBM_MINHASH_K; i++) {
        pos += snprintf(buf + pos, (size_t)(bufsize - pos), "%08x", fp->values[i]);
    }
}

bool cbm_minhash_from_hex(const char *hex, cbm_minhash_t *out) {
    if (!hex || !out) {
        return false;
    }
    size_t len = strlen(hex);
    if ((int)len != CBM_MINHASH_HEX_LEN) {
        return false;
    }
    for (int i = 0; i < CBM_MINHASH_K; i++) {
        char chunk[HEX_CHARS_PER_U32 + SKIP_ONE];
        ptrdiff_t offset = (ptrdiff_t)i * HEX_CHARS_PER_U32;
        memcpy(chunk, hex + offset, HEX_CHARS_PER_U32);
        chunk[HEX_CHARS_PER_U32] = '\0';
        unsigned long val = strtoul(chunk, NULL, HEX_BASE);
        out->values[i] = (uint32_t)val;
    }
    return true;
}

/* ── LSH index ───────────────────────────────────────────────────── */

/* Each band bucket is a dynamic array of entry indices (not pointers —
 * the entries array may be reallocated during insert, invalidating ptrs). */
typedef struct {
    int *items; /* indices into idx->entries */
    int count;
    int cap;
} lsh_bucket_t;

/* Band hash table: 65536 buckets per band (16-bit hash of r=2 values). */
enum { LSH_BUCKET_COUNT = 65536, LSH_BUCKET_MASK = 65535 };

struct cbm_lsh_index {
    /* b=32 bands, each with LSH_BUCKET_COUNT buckets */
    lsh_bucket_t bands[CBM_LSH_BANDS][LSH_BUCKET_COUNT];
    /* All entries stored for lifetime management */
    cbm_lsh_entry_t *entries;
    int entry_count;
    int entry_cap;
    /* Candidate result buffer (reused across queries) */
    const cbm_lsh_entry_t **result_buf;
    int result_count;
    int result_cap;
};

/* Compute band hash for band `b` from a MinHash signature.
 * Uses r=2 consecutive values starting at position b*r. */
static uint32_t band_hash(const cbm_minhash_t *fp, int band) {
    int base = band * CBM_LSH_ROWS;
    /* Combine r=2 values into a single hash */
    uint32_t combined[CBM_LSH_ROWS];
    for (int r = 0; r < CBM_LSH_ROWS; r++) {
        combined[r] = fp->values[base + r];
    }
    uint64_t h = XXH3_64bits(combined, sizeof(combined));
    return (uint32_t)(h & LSH_BUCKET_MASK);
}

static void bucket_push(lsh_bucket_t *bucket, int entry_index) {
    if (bucket->count >= bucket->cap) {
        int new_cap = bucket->cap < BUCKET_INIT_CAP ? BUCKET_INIT_CAP : bucket->cap * GROW_FACTOR;
        int *new_items = realloc(bucket->items, (size_t)new_cap * sizeof(int));
        if (!new_items) {
            return;
        }
        bucket->items = new_items;
        bucket->cap = new_cap;
    }
    bucket->items[bucket->count++] = entry_index;
}

cbm_lsh_index_t *cbm_lsh_new(void) {
    cbm_lsh_index_t *idx = calloc(SKIP_ONE, sizeof(cbm_lsh_index_t));
    return idx;
}

void cbm_lsh_insert(cbm_lsh_index_t *idx, const cbm_lsh_entry_t *entry) {
    if (!idx || !entry || !entry->fingerprint) {
        return;
    }

    /* Store a copy of the entry */
    if (idx->entry_count >= idx->entry_cap) {
        int new_cap =
            idx->entry_cap < ENTRY_INIT_CAP ? ENTRY_INIT_CAP : idx->entry_cap * GROW_FACTOR;
        cbm_lsh_entry_t *new_entries =
            realloc(idx->entries, (size_t)new_cap * sizeof(cbm_lsh_entry_t));
        if (!new_entries) {
            return;
        }
        idx->entries = new_entries;
        idx->entry_cap = new_cap;
    }
    int entry_idx = idx->entry_count;
    idx->entries[entry_idx] = *entry;
    idx->entry_count++;

    /* Insert index into each band's bucket */
    for (int b = 0; b < CBM_LSH_BANDS; b++) {
        uint32_t h = band_hash(entry->fingerprint, b);
        bucket_push(&idx->bands[b][h], entry_idx);
    }
}

/* Check if a node_id is already in the result buffer. */
static bool result_contains(const cbm_lsh_index_t *idx, int64_t node_id) {
    for (int j = 0; j < idx->result_count; j++) {
        if (idx->result_buf[j]->node_id == node_id) {
            return true;
        }
    }
    return false;
}

/* Append a candidate to the result buffer, growing if needed.  Returns false on OOM. */
static bool result_push(cbm_lsh_index_t *idx, const cbm_lsh_entry_t *candidate) {
    if (idx->result_count >= idx->result_cap) {
        int new_cap =
            idx->result_cap < RESULT_INIT_CAP ? RESULT_INIT_CAP : idx->result_cap * GROW_FACTOR;
        const cbm_lsh_entry_t **new_buf =
            realloc(idx->result_buf, (size_t)new_cap * sizeof(const cbm_lsh_entry_t *));
        if (!new_buf) {
            return false;
        }
        idx->result_buf = new_buf;
        idx->result_cap = new_cap;
    }
    idx->result_buf[idx->result_count++] = candidate;
    return true;
}

void cbm_lsh_query(const cbm_lsh_index_t *idx, const cbm_minhash_t *fp,
                   const cbm_lsh_entry_t ***out, int *count) {
    *out = NULL;
    *count = 0;

    if (!idx || !fp) {
        return;
    }

    /* Cast away const for result buffer management — query is logically const */
    cbm_lsh_index_t *mut_idx = (cbm_lsh_index_t *)idx;
    mut_idx->result_count = 0;

    for (int b = 0; b < CBM_LSH_BANDS; b++) {
        uint32_t h = band_hash(fp, b);
        const lsh_bucket_t *bucket = &idx->bands[b][h];
        for (int i = 0; i < bucket->count; i++) {
            const cbm_lsh_entry_t *candidate = &idx->entries[bucket->items[i]];
            if (result_contains(idx, candidate->node_id)) {
                continue;
            }
            if (!result_push(mut_idx, candidate)) {
                break;
            }
        }
    }

    *out = mut_idx->result_buf;
    *count = mut_idx->result_count;
}

void cbm_lsh_free(cbm_lsh_index_t *idx) {
    if (!idx) {
        return;
    }
    /* Free bucket arrays */
    for (int b = 0; b < CBM_LSH_BANDS; b++) {
        for (int h = 0; h < LSH_BUCKET_COUNT; h++) {
            free(idx->bands[b][h].items);
        }
    }
    free(idx->entries);
    free(idx->result_buf);
    free(idx);
}
