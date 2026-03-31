/*
 * httplink.c — HTTP route discovery and cross-service linking.
 *
 * Port of Go internal/httplink package.
 */

#include "pipeline/httplink.h"
#include "foundation/hash_table.h"
#include "foundation/yaml.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/compat_regex.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ─────────────────────────────────────────────────── */

/* Confidence band thresholds */
#define CONF_BAND_HIGH 0.7
#define CONF_BAND_MEDIUM 0.45
#define CONF_BAND_SPECULATIVE 0.25

/* Path match confidence */
#define CONF_PATH_EXACT 0.95
#define CONF_PATH_SUFFIX 0.75

/* Depth normalization factor */
#define DEPTH_DIVISOR 3.0

/* Default minimum confidence for filtering */
#define DEFAULT_MIN_CONFIDENCE 0.25

/* Buffer size guard: sizeof(buf) - 2 for trailing NUL + safety */
#define NORM_BUF_GUARD 1022
#define PARAM_BUF_GUARD 1020
#define HALF_BUF_GUARD 510

/* Receiver buffer max index */
#define RECEIVER_MAX 63

/* ── Similarity ────────────────────────────────────────────────── */

static int imin(int a, int b) {
    return a < b ? a : b;
}
static int imax(int a, int b) {
    return a > b ? a : b;
}

int cbm_levenshtein_distance(const char *a, const char *b) {
    int la = (int)strlen(a);
    int lb = (int)strlen(b);
    if (la == 0) {
        return lb;
    }
    if (lb == 0) {
        return la;
    }

    int *prev = calloc((size_t)lb + 1, sizeof(int));
    int *curr = calloc((size_t)lb + 1, sizeof(int));
    if (!prev || !curr) {
        free(prev);
        free(curr);
        return imax(la, lb);
    }

    for (int j = 0; j <= lb; j++) {
        prev[j] = j;
    }

    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = imin(curr[j - 1] + 1, imin(prev[j] + 1, prev[j - 1] + cost));
        }
        int *tmp = prev;
        prev = curr;
        curr = tmp;
    }
    int result = prev[lb];
    free(prev);
    free(curr);
    return result;
}

double cbm_normalized_levenshtein(const char *a, const char *b) {
    if (strcmp(a, b) == 0) {
        return 1.0;
    }
    int la = (int)strlen(a);
    int lb = (int)strlen(b);
    int max_len = imax(la, lb);
    if (max_len == 0) {
        return 1.0;
    }
    int dist = cbm_levenshtein_distance(a, b);
    return 1.0 - ((double)dist / (double)max_len);
}

static void free_ht_key(const char *key, void *val, void *ud) {
    (void)val;
    (void)ud;
    free((void *)key);
}

double cbm_ngram_overlap(const char *a, const char *b, int n) {
    int la = (int)strlen(a);
    int lb = (int)strlen(b);
    if (la < n || lb < n) {
        return 0.0;
    }

    int na = la - n + 1;
    int nb = lb - n + 1;

    /* Build set A using hash table for O(1) dedup */
    CBMHashTable *set_a = cbm_ht_create(na);
    for (int i = 0; i < na; i++) {
        char key[64];
        int klen = imin(n, (int)sizeof(key) - 1);
        memcpy(key, a + i, (size_t)klen);
        key[klen] = '\0';
        if (!cbm_ht_get(set_a, key)) {
            cbm_ht_set(set_a, strdup(key), (void *)1);
        }
    }

    /* Build set B using hash table, count intersection with A */
    CBMHashTable *set_b = cbm_ht_create(nb);
    int intersection = 0;
    for (int i = 0; i < nb; i++) {
        char key[64];
        int klen = imin(n, (int)sizeof(key) - 1);
        memcpy(key, b + i, (size_t)klen);
        key[klen] = '\0';
        if (!cbm_ht_get(set_b, key)) {
            cbm_ht_set(set_b, strdup(key), (void *)1);
            if (cbm_ht_get(set_a, key)) {
                intersection++;
            }
        }
    }

    int unique_a = (int)cbm_ht_count(set_a);
    int unique_b = (int)cbm_ht_count(set_b);

    /* Free hash table keys */
    cbm_ht_foreach(set_a, free_ht_key, NULL);
    cbm_ht_free(set_a);
    cbm_ht_foreach(set_b, free_ht_key, NULL);
    cbm_ht_free(set_b);

    int min_size = imin(unique_a, unique_b);
    if (min_size == 0) {
        return 0.0;
    }
    return (double)intersection / (double)min_size;
}

const char *cbm_confidence_band(double score) {
    if (score >= CONF_BAND_HIGH) {
        return "high";
    }
    if (score >= CONF_BAND_MEDIUM) {
        return "medium";
    }
    if (score >= CONF_BAND_SPECULATIVE) {
        return "speculative";
    }
    return "";
}

/* ── Path matching ─────────────────────────────────────────────── */

/* Strip URL scheme + host, returning pointer to path portion.
 * Returns NULL if URL has no path component. */
static char *strip_scheme_host(char *work) {
    char *path_start = work;
    char *scheme = strstr(work, "://");
    if (scheme) {
        path_start = scheme + 3;
        while (*path_start && *path_start != '/') {
            path_start++;
        }
        if (!*path_start) {
            return NULL;
        }
    }
    return path_start;
}

/* Replace :param and {param} path segments with wildcard '*'. */
static int replace_path_params(const char *path_start, int plen, char *out, int out_max) {
    int out_len = 0;
    for (int i = 0; i < plen && out_len < out_max;) {
        if (path_start[i] == ':' && (i == 0 || path_start[i - 1] == '/')) {
            out[out_len++] = '*';
            i++;
            while (i < plen && path_start[i] != '/') {
                i++;
            }
        } else if (path_start[i] == '{') {
            out[out_len++] = '*';
            i++;
            while (i < plen && path_start[i] != '}') {
                i++;
            }
            if (i < plen) {
                i++;
            }
        } else {
            out[out_len++] = path_start[i++];
        }
    }
    out[out_len] = '\0';
    return out_len;
}

const char *cbm_normalize_path(const char *input) {
    static CBM_TLS char buf[1024];
    if (!input || !*input) {
        buf[0] = '\0';
        return buf;
    }

    /* Copy and lowercase */
    char work[1024] = {0};
    int len = 0;
    for (int i = 0; input[i] && len < NORM_BUF_GUARD; i++) {
        work[len++] = (char)tolower((unsigned char)input[i]);
    }
    work[len] = '\0';

    char *path_start = strip_scheme_host(work);
    if (!path_start) {
        buf[0] = '\0';
        return buf;
    }

    /* Strip trailing slash */
    int plen = (int)strlen(path_start);
    if (plen > 1 && path_start[plen - 1] == '/') {
        path_start[--plen] = '\0';
    }

    replace_path_params(path_start, plen, buf, PARAM_BUF_GUARD);

    if (strcmp(buf, "/") == 0) {
        buf[0] = '\0';
    }

    return buf;
}

/* Normalize a path into a caller-owned buffer for comparison */
static void normalize_to_buf(const char *input, char *out, int out_size) {
    const char *norm = cbm_normalize_path(input);
    int len = (int)strlen(norm);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, norm, (size_t)len);
    out[len] = '\0';
}

/* Compare two segment arrays with wildcard matching.
 * a_segs[a_off..a_off+count-1] vs b_segs[0..count-1]. */
static bool segments_match_wildcard(char **a_segs, int a_off, char **b_segs, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(a_segs[a_off + i], "*") == 0 || strcmp(b_segs[i], "*") == 0) {
            continue;
        }
        if (strcmp(a_segs[a_off + i], b_segs[i]) != 0) {
            return false;
        }
    }
    return true;
}

bool cbm_paths_match(const char *call_path, const char *route_path) {
    char norm_call[1024];
    char norm_route[1024];
    normalize_to_buf(call_path, norm_call, sizeof(norm_call));
    normalize_to_buf(route_path, norm_route, sizeof(norm_route));

    if (!*norm_call || !*norm_route) {
        return false;
    }

    if (strcmp(norm_call, norm_route) == 0) {
        return true;
    }

    int lc = (int)strlen(norm_call);
    int lr = (int)strlen(norm_route);
    if (lc > lr && strcmp(norm_call + lc - lr, norm_route) == 0) {
        return true;
    }

    char call_copy[1024];
    char route_copy[1024];
    strncpy(call_copy, norm_call, sizeof(call_copy) - 1);
    call_copy[sizeof(call_copy) - 1] = '\0';

    char *call_segs[64];
    char *route_segs[64];
    int nc = 0;
    int nr = 0;

    char *tok = strtok(call_copy, "/");
    while (tok && nc < 64) {
        call_segs[nc++] = tok;
        tok = strtok(NULL, "/");
    }

    normalize_to_buf(route_path, route_copy, sizeof(route_copy));
    tok = strtok(route_copy, "/");
    while (tok && nr < 64) {
        route_segs[nr++] = tok;
        tok = strtok(NULL, "/");
    }

    if (nc == nr && segments_match_wildcard(call_segs, 0, route_segs, nc)) {
        return true;
    }

    if (nc > nr && segments_match_wildcard(call_segs, nc - nr, route_segs, nr)) {
        return true;
    }

    return false;
}

/* Count segments in a normalized path */
static int count_segments(const char *path) {
    int count = 0;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') {
            count++;
        }
    }
    return count;
}

/* Jaccard similarity of path segments (intersection/union) */
static double segment_jaccard(const char *norm_call, const char *norm_route) {
    /* Split into segments */
    char a[1024];
    char b[1024];
    strncpy(a, norm_call, sizeof(a) - 1);
    a[sizeof(a) - 1] = '\0';
    strncpy(b, norm_route, sizeof(b) - 1);
    b[sizeof(b) - 1] = '\0';

    char *a_segs[64];
    char *b_segs[64];
    int na = 0;
    int nb = 0;

    char *t = strtok(a, "/");
    while (t && na < 64) {
        a_segs[na++] = t;
        t = strtok(NULL, "/");
    }

    t = strtok(b, "/");
    while (t && nb < 64) {
        b_segs[nb++] = t;
        t = strtok(NULL, "/");
    }

    if (na == 0 && nb == 0) {
        return 1.0;
    }
    if (na == 0 || nb == 0) {
        return 0.0;
    }

    /* Count intersection (match segments in order, accounting for wildcards) */
    int intersect = 0;
    int max_len = imax(na, nb);
    int min_len = imin(na, nb);
    for (int i = 0; i < min_len; i++) {
        if (strcmp(a_segs[i], b_segs[i]) == 0 || strcmp(a_segs[i], "*") == 0 ||
            strcmp(b_segs[i], "*") == 0) {
            intersect++;
        }
    }

    return (double)intersect / (double)max_len;
}

/* Determine match type between two normalized paths.
 * Returns base score (CONF_PATH_EXACT, CONF_PATH_SUFFIX, or 0.0).
 * Sets *is_suffix if it was a suffix match. */
static double classify_path_match(const char *norm_call, const char *norm_route, bool *is_suffix) {
    *is_suffix = false;

    if (strcmp(norm_call, norm_route) == 0) {
        return CONF_PATH_EXACT;
    }

    int lc = (int)strlen(norm_call);
    int lr = (int)strlen(norm_route);
    if (lc > lr && strcmp(norm_call + lc - lr, norm_route) == 0) {
        *is_suffix = true;
        return CONF_PATH_SUFFIX;
    }

    /* Segment-wise match with wildcards */
    char c2[1024];
    char r2[1024];
    strncpy(c2, norm_call, sizeof(c2) - 1);
    c2[sizeof(c2) - 1] = '\0';
    strncpy(r2, norm_route, sizeof(r2) - 1);
    r2[sizeof(r2) - 1] = '\0';

    char *cs[64];
    char *rs[64];
    int nc2 = 0;
    int nr2 = 0;
    char *tk = strtok(c2, "/");
    while (tk && nc2 < 64) {
        cs[nc2++] = tk;
        tk = strtok(NULL, "/");
    }
    tk = strtok(r2, "/");
    while (tk && nr2 < 64) {
        rs[nr2++] = tk;
        tk = strtok(NULL, "/");
    }

    if (nc2 == nr2 && segments_match_wildcard(cs, 0, rs, nc2)) {
        return CONF_PATH_EXACT;
    }

    return 0.0;
}

double cbm_path_match_score(const char *call_path, const char *route_path) {
    char norm_call[1024];
    char norm_route[1024];
    normalize_to_buf(call_path, norm_call, sizeof(norm_call));
    normalize_to_buf(route_path, norm_route, sizeof(norm_route));

    if (!*norm_call || !*norm_route) {
        return 0.0;
    }

    bool is_suffix = false;
    double match_base = classify_path_match(norm_call, norm_route, &is_suffix);
    if (match_base == 0.0) {
        return 0.0;
    }

    double jaccard = segment_jaccard(norm_call, norm_route);
    int depth = count_segments(is_suffix ? norm_route : norm_call);
    double depth_factor = (double)depth / DEPTH_DIVISOR;
    if (depth_factor > 1.0) {
        depth_factor = 1.0;
    }

    double confidence = (0.5 * jaccard) + (0.5 * depth_factor);
    return match_base * confidence;
}

bool cbm_same_service(const char *qn1, const char *qn2) {
    /* Split QN by '.', strip last 2 segments (module+name), compare rest */
    char a[1024];
    char b[1024];
    strncpy(a, qn1, sizeof(a) - 1);
    a[sizeof(a) - 1] = '\0';
    strncpy(b, qn2, sizeof(b) - 1);
    b[sizeof(b) - 1] = '\0';

    /* Count segments */
    char *a_segs[64];
    char *b_segs[64];
    int na = 0;
    int nb = 0;

    char *tok = strtok(a, ".");
    while (tok && na < 64) {
        a_segs[na++] = tok;
        tok = strtok(NULL, ".");
    }

    tok = strtok(b, ".");
    while (tok && nb < 64) {
        b_segs[nb++] = tok;
        tok = strtok(NULL, ".");
    }

    /* Need at least 3 segments (dir + module + name) */
    if (na < 3 || nb < 3) {
        return false;
    }

    /* Strip last 2 segments */
    int da = na - 2;
    int db = nb - 2;

    if (da != db) {
        return false;
    }

    for (int i = 0; i < da; i++) {
        if (strcmp(a_segs[i], b_segs[i]) != 0) {
            return false;
        }
    }
    return true;
}

/* ── Detection ─────────────────────────────────────────────────── */

const char *cbm_detect_protocol(const char *source) {
    if (!source || !*source) {
        return "";
    }

    /* WebSocket patterns */
    if (strstr(source, "websocket.Upgrade") || strstr(source, "websocket.Accept") ||
        strstr(source, "upgrader.Upgrade") || strstr(source, "ws.on(") ||
        strstr(source, "io.on(")) {
        return "ws";
    }

    /* SSE patterns */
    if (strstr(source, "text/event-stream") || strstr(source, "EventSourceResponse") ||
        strstr(source, "SseEmitter") || strstr(source, "ServerSentEvent")) {
        return "sse";
    }

    return "";
}

/* containsTestSegment: check if path has a segment that equals testWord */
static bool contains_test_segment(const char *fp, const char *test_word) {
    char work[1024];
    int len = (int)strlen(fp);
    if (len >= (int)sizeof(work)) {
        len = (int)sizeof(work) - 1;
    }
    memcpy(work, fp, (size_t)len);
    work[len] = '\0';

    /* Replace / with \0 and check each segment */
    char *p = work;
    while (*p) {
        char *seg = p;
        while (*p && *p != '/') {
            p++;
        }
        char saved = *p;
        *p = '\0';
        if (strcmp(seg, test_word) == 0) {
            return true;
        }
        if (saved) {
            *p = saved;
            p++;
        } else {
            { break; }
        }
    }
    return false;
}

bool cbm_is_test_node_fp(const char *file_path, bool is_test_prop) {
    if (is_test_prop) {
        return true;
    }
    if (!file_path || !*file_path) {
        return false;
    }

    /* Convert to forward slashes for consistent matching */
    char fp[1024];
    int len = 0;
    for (int i = 0; file_path[i] && len < NORM_BUF_GUARD; i++) {
        fp[len++] = (file_path[i] == '\\') ? '/' : file_path[i];
    }
    fp[len] = '\0';

    if (contains_test_segment(fp, "test")) {
        return true;
    }
    if (contains_test_segment(fp, "tests")) {
        return true;
    }
    if (contains_test_segment(fp, "__tests__")) {
        return true;
    }
    if (strstr(fp, "_test.")) {
        return true;
    }
    if (strstr(fp, ".test.")) {
        return true;
    }
    if (strstr(fp, "spec/")) {
        return true;
    }
    if (strstr(fp, "fixtures/")) {
        return true;
    }
    return false;
}

bool cbm_is_path_excluded(const char *path, const char **exclude_paths, int count) {
    if (!path || !*path) {
        return false;
    }

    /* Normalize: lowercase, strip trailing / */
    char norm[512];
    int len = 0;
    for (int i = 0; path[i] && len < HALF_BUF_GUARD; i++) {
        norm[len++] = (char)tolower((unsigned char)path[i]);
    }
    norm[len] = '\0';
    if (len > 1 && norm[len - 1] == '/') {
        norm[--len] = '\0';
    }

    for (int i = 0; i < count; i++) {
        /* Normalize exclude path too */
        char excl[512];
        int elen = 0;
        for (int j = 0; exclude_paths[i][j] && elen < HALF_BUF_GUARD; j++) {
            excl[elen++] = (char)tolower((unsigned char)exclude_paths[i][j]);
        }
        excl[elen] = '\0';
        if (elen > 1 && excl[elen - 1] == '/') {
            excl[--elen] = '\0';
        }

        if (strcmp(norm, excl) == 0) {
            return true;
        }
    }
    return false;
}

/* ── URL extraction ────────────────────────────────────────────── */

int cbm_extract_url_paths(const char *text, char **out, int max_out) {
    if (!text || !*text) {
        return 0;
    }

    int count = 0;
    cbm_regex_t url_re;
    cbm_regex_t path_re;

    /* URL pattern: https?://host/path */
    if (cbm_regcomp(&url_re, "https?://[a-zA-Z0-9.\\-]+(/[a-zA-Z0-9_/:.\\.\\-]+)",
                    CBM_REG_EXTENDED) != 0) {
        return 0;
    }

    /* Path pattern: "/path" (quoted paths starting with /) */
    if (cbm_regcomp(&path_re, "[\"'](/[a-zA-Z0-9_/:.\\.\\-]{2,})[\"']", CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&url_re);
        return 0;
    }

    /* Search for URL patterns */
    const char *p = text;
    cbm_regmatch_t match[2];

    while (count < max_out && cbm_regexec(&url_re, p, 2, match, 0) == 0) {
        int path_start = match[1].rm_so;
        int path_end = match[1].rm_eo;
        int path_len = path_end - path_start;
        if (path_len > 0 && path_len < 256) {
            out[count] = malloc((size_t)path_len + 1);
            memcpy(out[count], p + path_start, (size_t)path_len);
            out[count][path_len] = '\0';
            count++;
        }
        p += match[0].rm_eo;
    }

    /* Search for path-only patterns */
    p = text;
    while (count < max_out && cbm_regexec(&path_re, p, 2, match, 0) == 0) {
        int path_start = match[1].rm_so;
        int path_end = match[1].rm_eo;
        int path_len = path_end - path_start;
        if (path_len > 0 && path_len < 256) {
            /* Check not already captured as part of a URL */
            char *path_str = malloc((size_t)path_len + 1);
            memcpy(path_str, p + path_start, (size_t)path_len);
            path_str[path_len] = '\0';

            bool dup = false;
            for (int i = 0; i < count; i++) {
                if (strcmp(out[i], path_str) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                out[count++] = path_str;
            } else {
                free(path_str);
            }
        }
        p += match[0].rm_eo;
    }

    cbm_regfree(&url_re);
    cbm_regfree(&path_re);
    return count;
}

int cbm_extract_json_string_paths(const char *text, char **out, int max_out) {
    if (!text || !*text) {
        return 0;
    }

    /* Look for JSON-like strings containing URLs or paths */
    /* Extract all quoted strings and check for URL/path patterns */
    int count = 0;

    /* First try extracting URLs from the raw text (covers JSON string values) */
    count = cbm_extract_url_paths(text, out, max_out);

    return count;
}

/* ── Route extraction: Python ──────────────────────────────────── */

/* Fill common route fields (function_name, qualified_name). */
static void fill_route_identity(cbm_route_handler_t *r, const char *name, const char *qn) {
    strncpy(r->function_name, name ? name : "", sizeof(r->function_name) - 1);
    strncpy(r->qualified_name, qn ? qn : "", sizeof(r->qualified_name) - 1);
}

/* Copy a regex match group into a buffer, uppercase it, return length. */
static int copy_match_upper(char *dst, int dst_size, const char *src, int so, int eo) {
    int len = eo - so;
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src + so, (size_t)len);
    dst[len] = '\0';
    for (int j = 0; dst[j]; j++) {
        dst[j] = (char)toupper((unsigned char)dst[j]);
    }
    return len;
}

/* Copy a regex match group into a buffer, return length. */
static int copy_match(char *dst, int dst_size, const char *src, int so, int eo) {
    int len = eo - so;
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src + so, (size_t)len);
    dst[len] = '\0';
    return len;
}

int cbm_extract_python_routes(const char *name, const char *qn, const char **decorators, int ndec,
                              cbm_route_handler_t *out, int max_out) {
    if (!decorators || ndec <= 0) {
        return 0;
    }

    cbm_regex_t py_route_re;
    cbm_regex_t py_ws_re;
    if (cbm_regcomp(&py_route_re,
                    "@[a-zA-Z_]+\\.(get|post|put|delete|patch)\\([[:space:]]*[\"']([^\"']*)[\"']",
                    CBM_REG_EXTENDED) != 0) {
        return 0;
    }
    if (cbm_regcomp(&py_ws_re, "@[a-zA-Z_]+\\.websocket\\([[:space:]]*[\"']([^\"']*)[\"']",
                    CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&py_route_re);
        return 0;
    }

    int count = 0;
    for (int i = 0; i < ndec && count < max_out; i++) {
        cbm_regmatch_t match[3];

        if (cbm_regexec(&py_ws_re, decorators[i], 2, match, 0) == 0) {
            cbm_route_handler_t *r = &out[count];
            memset(r, 0, sizeof(*r));
            copy_match(r->path, (int)sizeof(r->path), decorators[i], match[1].rm_so,
                       match[1].rm_eo);
            strncpy(r->method, "WS", sizeof(r->method) - 1);
            strncpy(r->protocol, "ws", sizeof(r->protocol) - 1);
            fill_route_identity(r, name, qn);
            count++;
            continue;
        }

        if (cbm_regexec(&py_route_re, decorators[i], 3, match, 0) == 0) {
            cbm_route_handler_t *r = &out[count];
            memset(r, 0, sizeof(*r));
            copy_match_upper(r->method, (int)sizeof(r->method), decorators[i], match[1].rm_so,
                             match[1].rm_eo);
            copy_match(r->path, (int)sizeof(r->path), decorators[i], match[2].rm_so,
                       match[2].rm_eo);
            fill_route_identity(r, name, qn);
            count++;
        }
    }

    cbm_regfree(&py_route_re);
    cbm_regfree(&py_ws_re);
    return count;
}

/* ── Route extraction: Go gin/chi ──────────────────────────────── */

/* Gin Group() binding: var → prefix. */
typedef struct {
    char var[64];
    char prefix[256];
} go_gin_group_t;

/* Chi Route() match position + prefix. */
typedef struct {
    int end_pos;
    char prefix[256];
} go_chi_match_t;

/* Route regex match with byte positions. */
typedef struct {
    int start_pos, end_pos;
    int method_so, method_eo;
    int path_so, path_eo;
    int handler_so, handler_eo;
} go_route_match_t;

/* Collect gin Group() bindings from source. */
static int collect_gin_groups(const char *source, const cbm_regex_t *re, go_gin_group_t *out,
                              int max) {
    int count = 0;
    const char *sp = source;
    cbm_regmatch_t gm[4];
    while (cbm_regexec(re, sp, 4, gm, 0) == 0 && count < max) {
        int vlen = (gm[1].rm_eo - gm[1].rm_so);
        int plen = (gm[3].rm_eo - gm[3].rm_so);
        if (vlen < 64 && plen < 256) {
            memcpy(out[count].var, sp + gm[1].rm_so, (size_t)vlen);
            out[count].var[vlen] = '\0';
            memcpy(out[count].prefix, sp + gm[3].rm_so, (size_t)plen);
            out[count].prefix[plen] = '\0';
            count++;
        }
        sp += gm[0].rm_eo;
    }
    return count;
}

/* Collect chi Route() matches with absolute byte positions. */
static int collect_chi_matches(const char *source, const cbm_regex_t *re, go_chi_match_t *out,
                               int max) {
    int count = 0;
    const char *sp = source;
    cbm_regmatch_t cm[2];
    while (cbm_regexec(re, sp, 2, cm, 0) == 0 && count < max) {
        int abs_end = (int)((sp - source) + cm[0].rm_eo);
        int plen = (cm[1].rm_eo - cm[1].rm_so);
        if (plen < 256) {
            out[count].end_pos = abs_end;
            memcpy(out[count].prefix, sp + cm[1].rm_so, (size_t)plen);
            out[count].prefix[plen] = '\0';
            count++;
        }
        sp += cm[0].rm_eo;
    }
    return count;
}

/* Collect route regex matches with absolute byte positions. */
static int collect_route_matches(const char *source, const cbm_regex_t *re, go_route_match_t *out,
                                 int max) {
    int count = 0;
    const char *sp = source;
    cbm_regmatch_t rm[4];
    while (cbm_regexec(re, sp, 4, rm, 0) == 0 && count < max) {
        int base = (int)(sp - source);
        out[count].start_pos = (base + rm[0].rm_so);
        out[count].end_pos = (base + rm[0].rm_eo);
        out[count].method_so = (base + rm[1].rm_so);
        out[count].method_eo = (base + rm[1].rm_eo);
        out[count].path_so = (base + rm[2].rm_so);
        out[count].path_eo = (base + rm[2].rm_eo);
        out[count].handler_so = (rm[3].rm_so >= 0) ? (base + rm[3].rm_so) : -1;
        out[count].handler_eo = (rm[3].rm_eo >= 0) ? (base + rm[3].rm_eo) : -1;
        count++;
        sp += rm[0].rm_eo;
    }
    return count;
}

/* Chi prefix stack entry. */
typedef struct {
    char prefix[256];
    int depth;
} chi_stack_entry_t;

/* Concatenate all active chi prefixes into a route prefix. */
static void build_chi_prefix(const chi_stack_entry_t *stack, int top, char *out, int out_sz) {
    out[0] = '\0';
    for (int s = 0; s < top; s++) {
        int cur = (int)strlen(out);
        int pf = (int)strlen(stack[s].prefix);
        if (cur + pf < out_sz - 1) {
            strcat(out, stack[s].prefix);
        }
    }
}

/* Handle brace tracking for chi prefix computation. */
static void chi_handle_brace(char c, chi_stack_entry_t *stack, int *top, int *brace_depth,
                             bool *pending, const char *pending_prefix) {
    if (c == '{') {
        (*brace_depth)++;
        if (*pending && *top < 32) {
            strncpy(stack[*top].prefix, pending_prefix, sizeof(stack[*top].prefix) - 1);
            stack[*top].prefix[sizeof(stack[*top].prefix) - 1] = '\0';
            stack[*top].depth = *brace_depth;
            (*top)++;
            *pending = false;
        }
    } else if (c == '}') {
        while (*top > 0 && stack[*top - 1].depth == *brace_depth) {
            (*top)--;
        }
        (*brace_depth)--;
    }
}

/* Compute chi prefix for each route via brace-depth tracking. */
static void compute_chi_prefixes(const char *source, const go_chi_match_t *chi_matches, int nchi,
                                 const go_route_match_t *route_matches, int nmatches,
                                 char (*route_chi_prefix)[512]) {
    if (nchi == 0 || nmatches == 0) {
        return;
    }

    chi_stack_entry_t chi_stack[32];
    int chi_top = 0;
    int brace_depth = 0;
    int next_chi = 0;
    int next_route = 0;
    bool pending = false;
    char pending_prefix[256] = "";

    int src_len = (int)strlen(source);
    for (int i = 0; i <= src_len && next_route < nmatches; i++) {
        while (next_chi < nchi && i >= chi_matches[next_chi].end_pos) {
            pending = true;
            strncpy(pending_prefix, chi_matches[next_chi].prefix, sizeof(pending_prefix) - 1);
            pending_prefix[sizeof(pending_prefix) - 1] = '\0';
            next_chi++;
        }

        if (i < src_len) {
            chi_handle_brace(source[i], chi_stack, &chi_top, &brace_depth, &pending,
                             pending_prefix);
        }

        while (next_route < nmatches && i == route_matches[next_route].start_pos) {
            build_chi_prefix(chi_stack, chi_top, route_chi_prefix[next_route], 512);
            next_route++;
        }
    }
}

/* Extract handler ref from match, trimming whitespace and trailing chars. */
static void extract_handler_ref(const char *source, const go_route_match_t *rm,
                                cbm_route_handler_t *r) {
    if (rm->handler_so < 0) {
        return;
    }
    int hlen = rm->handler_eo - rm->handler_so;
    const char *hs = source + rm->handler_so;
    while (hlen > 0 && (hs[0] == ' ' || hs[0] == '\t')) {
        hs++;
        hlen--;
    }
    while (hlen > 0 && (hs[hlen - 1] == ' ' || hs[hlen - 1] == '\t' || hs[hlen - 1] == ')' ||
                        hs[hlen - 1] == '\n')) {
        hlen--;
    }
    if (hlen > 0 && hlen < (int)sizeof(r->handler_ref)) {
        memcpy(r->handler_ref, hs, (size_t)hlen);
        r->handler_ref[hlen] = '\0';
    }
}

/* Find receiver variable name before the dot in ".METHOD(" at start_pos. */
static void find_receiver(const char *source, int start_pos, char *receiver, int max_len) {
    receiver[0] = '\0';
    const char *dot_pos = source + start_pos;
    if (dot_pos <= source) {
        return;
    }
    const char *rp = dot_pos - 1;
    while (rp >= source && (isalnum((unsigned char)*rp) || *rp == '_')) {
        rp--;
    }
    rp++;
    int rlen = (int)(dot_pos - rp);
    if (rlen > 0 && rlen < max_len) {
        memcpy(receiver, rp, (size_t)rlen);
        receiver[rlen] = '\0';
    }
}

/* Apply gin group or chi prefix to a route path. */
static void apply_go_route_prefix(cbm_route_handler_t *r, const char *receiver,
                                  const go_gin_group_t *gin_groups, int ngin,
                                  const char *chi_prefix) {
    for (int g = 0; g < ngin; g++) {
        if (strcmp(receiver, gin_groups[g].var) == 0) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s%s", gin_groups[g].prefix, r->path);
            strncpy(r->path, full_path, sizeof(r->path) - 1);
            r->path[sizeof(r->path) - 1] = '\0';
            return;
        }
    }
    if (chi_prefix && chi_prefix[0]) {
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s%s", chi_prefix, r->path);
        strncpy(r->path, full_path, sizeof(r->path) - 1);
        r->path[sizeof(r->path) - 1] = '\0';
    }
}

int cbm_extract_go_routes(const char *name, const char *qn, const char *source,
                          cbm_route_handler_t *out, int max_out) {
    if (!source || !*source) {
        return 0;
    }

    cbm_regex_t go_route_re;
    cbm_regex_t go_group_re;
    cbm_regex_t go_chi_re;

    if (cbm_regcomp(
            &go_route_re,
            "\\.(GET|POST|PUT|DELETE|PATCH|Get|Post|Put|Delete|Patch)\\([[:space:]]*[\"']([^\"'"
            "]*)[\"'][[:space:]]*,[[:space:]]*([^,)]+)",
            CBM_REG_EXTENDED) != 0) {
        return 0;
    }
    if (cbm_regcomp(
            &go_group_re,
            "([a-zA-Z_][a-zA-Z0-9_]*)[[:space:]]*(:=|=)[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*\\."
            "Group\\([[:space:]]*[\"']([^\"']+)[\"']",
            CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&go_route_re);
        return 0;
    }
    if (cbm_regcomp(&go_chi_re, "\\.Route\\([[:space:]]*\"([^\"]+)\"[[:space:]]*,[[:space:]]*func",
                    CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&go_route_re);
        cbm_regfree(&go_group_re);
        return 0;
    }

    go_gin_group_t gin_groups[32];
    int ngin = collect_gin_groups(source, &go_group_re, gin_groups, 32);

    go_chi_match_t chi_matches[32];
    int nchi = collect_chi_matches(source, &go_chi_re, chi_matches, 32);

    go_route_match_t route_matches[64];
    int nmatches = collect_route_matches(source, &go_route_re, route_matches, 64);

    char route_chi_prefix[64][512];
    memset(route_chi_prefix, 0, sizeof(route_chi_prefix));
    compute_chi_prefixes(source, chi_matches, nchi, route_matches, nmatches, route_chi_prefix);

    int count = 0;
    for (int ri = 0; ri < nmatches && count < max_out; ri++) {
        cbm_route_handler_t *r = &out[count];
        memset(r, 0, sizeof(*r));

        copy_match_upper(r->method, (int)sizeof(r->method), source, route_matches[ri].method_so,
                         route_matches[ri].method_eo);
        copy_match(r->path, (int)sizeof(r->path), source, route_matches[ri].path_so,
                   route_matches[ri].path_eo);

        char receiver[64];
        find_receiver(source, route_matches[ri].start_pos, receiver, 64);
        apply_go_route_prefix(r, receiver, gin_groups, ngin, route_chi_prefix[ri]);

        fill_route_identity(r, name, qn);
        extract_handler_ref(source, &route_matches[ri], r);
        count++;
    }

    cbm_regfree(&go_route_re);
    cbm_regfree(&go_group_re);
    cbm_regfree(&go_chi_re);
    return count;
}

/* ── Route extraction: Java Spring ─────────────────────────────── */

/* Resolve Spring annotation name to HTTP method string. */
static const char *spring_method_from_annotation(const char *ann_name) {
    if (strcmp(ann_name, "Get") == 0) {
        return "GET";
    }
    if (strcmp(ann_name, "Post") == 0) {
        return "POST";
    }
    if (strcmp(ann_name, "Put") == 0) {
        return "PUT";
    }
    if (strcmp(ann_name, "Delete") == 0) {
        return "DELETE";
    }
    if (strcmp(ann_name, "Patch") == 0) {
        return "PATCH";
    }
    return "ANY";
}

int cbm_extract_java_routes(const char *name, const char *qn, const char **decorators, int ndec,
                            cbm_route_handler_t *out, int max_out) {
    if (!decorators || ndec <= 0) {
        return 0;
    }

    cbm_regex_t spring_re;
    cbm_regex_t spring_ws_re;
    if (cbm_regcomp(
            &spring_re,
            "@(Get|Post|Put|Delete|Patch|Request)Mapping\\([[:space:]]*(value[[:space:]]*=[[:"
            "space:]]*)?[\"']([^\"']+)[\"']",
            CBM_REG_EXTENDED) != 0) {
        return 0;
    }
    if (cbm_regcomp(&spring_ws_re, "@MessageMapping\\([[:space:]]*[\"']([^\"']+)[\"']",
                    CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&spring_re);
        return 0;
    }

    int count = 0;
    for (int i = 0; i < ndec && count < max_out; i++) {
        cbm_regmatch_t match[4];

        if (cbm_regexec(&spring_ws_re, decorators[i], 2, match, 0) == 0) {
            cbm_route_handler_t *r = &out[count];
            memset(r, 0, sizeof(*r));
            copy_match(r->path, (int)sizeof(r->path), decorators[i], match[1].rm_so,
                       match[1].rm_eo);
            strncpy(r->method, "WS", sizeof(r->method) - 1);
            strncpy(r->protocol, "ws", sizeof(r->protocol) - 1);
            fill_route_identity(r, name, qn);
            count++;
            continue;
        }

        if (cbm_regexec(&spring_re, decorators[i], 4, match, 0) == 0) {
            cbm_route_handler_t *r = &out[count];
            memset(r, 0, sizeof(*r));

            char method_name[32];
            copy_match(method_name, (int)sizeof(method_name), decorators[i], match[1].rm_so,
                       match[1].rm_eo);
            strncpy(r->method, spring_method_from_annotation(method_name), sizeof(r->method) - 1);

            copy_match(r->path, (int)sizeof(r->path), decorators[i], match[3].rm_so,
                       match[3].rm_eo);
            fill_route_identity(r, name, qn);
            count++;
        }
    }

    cbm_regfree(&spring_re);
    cbm_regfree(&spring_ws_re);
    return count;
}

/* ── Route extraction: Kotlin Ktor ─────────────────────────────── */

/* Collect Ktor WebSocket routes. */
static int collect_ktor_ws_routes(const char *source, const cbm_regex_t *re, const char *name,
                                  const char *qn, cbm_route_handler_t *out, int max_out) {
    int count = 0;
    const char *p = source;
    cbm_regmatch_t match[2];

    while (count < max_out && cbm_regexec(re, p, 2, match, 0) == 0) {
        cbm_route_handler_t *r = &out[count];
        memset(r, 0, sizeof(*r));
        copy_match(r->path, (int)sizeof(r->path), p, match[1].rm_so, match[1].rm_eo);
        strncpy(r->method, "WS", sizeof(r->method) - 1);
        strncpy(r->protocol, "ws", sizeof(r->protocol) - 1);
        fill_route_identity(r, name, qn);
        count++;
        p += match[0].rm_eo;
    }
    return count;
}

/* Collect Ktor HTTP routes, skipping webSocket false positives. */
static int collect_ktor_http_routes(const char *source, const cbm_regex_t *re, const char *name,
                                    const char *qn, cbm_route_handler_t *out, int max_out) {
    int count = 0;
    const char *p = source;
    cbm_regmatch_t match[3];

    while (count < max_out && cbm_regexec(re, p, 3, match, 0) == 0) {
        const char *match_start = p + match[0].rm_so;
        bool is_ws = (match_start >= source + 3 && strncmp(match_start - 3, "web", 3) == 0);

        if (!is_ws) {
            cbm_route_handler_t *r = &out[count];
            memset(r, 0, sizeof(*r));
            copy_match_upper(r->method, (int)sizeof(r->method), p, match[1].rm_so, match[1].rm_eo);
            copy_match(r->path, (int)sizeof(r->path), p, match[2].rm_so, match[2].rm_eo);
            fill_route_identity(r, name, qn);
            count++;
        }
        p += match[0].rm_eo;
    }
    return count;
}

int cbm_extract_ktor_routes(const char *name, const char *qn, const char *source,
                            cbm_route_handler_t *out, int max_out) {
    if (!source || !*source) {
        return 0;
    }

    cbm_regex_t ktor_re;
    cbm_regex_t ktor_ws_re;
    if (cbm_regcomp(&ktor_re,
                    "(get|post|put|delete|patch)\\([[:space:]]*\"([^\"]+)\"[[:space:]]*\\)",
                    CBM_REG_EXTENDED) != 0) {
        return 0;
    }
    if (cbm_regcomp(&ktor_ws_re, "webSocket\\([[:space:]]*\"([^\"]+)\"[[:space:]]*\\)",
                    CBM_REG_EXTENDED) != 0) {
        cbm_regfree(&ktor_re);
        return 0;
    }

    int count = collect_ktor_ws_routes(source, &ktor_ws_re, name, qn, out, max_out);
    count += collect_ktor_http_routes(source, &ktor_re, name, qn, out + count, max_out - count);

    cbm_regfree(&ktor_re);
    cbm_regfree(&ktor_ws_re);
    return count;
}

/* ── Route extraction: Express.js ──────────────────────────────── */

static bool is_express_receiver(const char *receiver) {
    static const char *allowlist[] = {"app",    "router",  "server", "api",
                                      "routes", "express", "route"};
    for (int i = 0; i < 7; i++) {
        if (strcmp(receiver, allowlist[i]) == 0) {
            return true;
        }
    }
    return false;
}

int cbm_extract_express_routes(const char *name, const char *qn, const char *source,
                               cbm_route_handler_t *out, int max_out) {
    if (!source || !*source) {
        return 0;
    }

    cbm_regex_t express_re;
    if (cbm_regcomp(
            &express_re,
            "([a-zA-Z_][a-zA-Z0-9_]*)\\.(get|post|put|delete|patch)\\([[:space:]]*[\"'`]([^\"'`"
            "]+)[\"'`]",
            CBM_REG_EXTENDED) != 0) {
        return 0;
    }

    int count = 0;
    const char *p = source;
    cbm_regmatch_t match[4];

    while (count < max_out && cbm_regexec(&express_re, p, 4, match, 0) == 0) {
        char receiver[64];
        copy_match(receiver, (int)sizeof(receiver), p, match[1].rm_so, match[1].rm_eo);

        if (is_express_receiver(receiver)) {
            cbm_route_handler_t *r = &out[count];
            memset(r, 0, sizeof(*r));
            copy_match_upper(r->method, (int)sizeof(r->method), p, match[2].rm_so, match[2].rm_eo);
            copy_match(r->path, (int)sizeof(r->path), p, match[3].rm_so, match[3].rm_eo);
            fill_route_identity(r, name, qn);
            count++;
        }
        p += match[0].rm_eo;
    }

    cbm_regfree(&express_re);
    return count;
}

/* ── Route extraction: Laravel ─────────────────────────────────── */

int cbm_extract_laravel_routes(const char *name, const char *qn, const char *source,
                               cbm_route_handler_t *out, int max_out) {
    if (!source || !*source) {
        return 0;
    }

    cbm_regex_t laravel_re;
    if (cbm_regcomp(&laravel_re,
                    "Route::(get|post|put|delete|patch)\\([[:space:]]*[\"']([^\"']+)[\"']",
                    CBM_REG_EXTENDED) != 0) {
        return 0;
    }

    int count = 0;
    const char *p = source;
    cbm_regmatch_t match[3];

    while (count < max_out && cbm_regexec(&laravel_re, p, 3, match, 0) == 0) {
        cbm_route_handler_t *r = &out[count];
        memset(r, 0, sizeof(*r));

        /* Method */
        int mlen = (match[1].rm_eo - match[1].rm_so);
        if (mlen >= (int)sizeof(r->method)) {
            mlen = (int)sizeof(r->method) - 1;
        }
        memcpy(r->method, p + match[1].rm_so, (size_t)mlen);
        r->method[mlen] = '\0';
        for (int j = 0; r->method[j]; j++) {
            r->method[j] = (char)toupper((unsigned char)r->method[j]);
        }

        /* Path */
        int plen = (match[2].rm_eo - match[2].rm_so);
        if (plen >= (int)sizeof(r->path)) {
            plen = (int)sizeof(r->path) - 1;
        }
        memcpy(r->path, p + match[2].rm_so, (size_t)plen);
        r->path[plen] = '\0';

        /* Skip non-route strings that match the regex but contain characters
         * invalid in URL paths (e.g., cache keys, interpolated expressions).
         * Laravel route parameters use {param} syntax, so valid routes pass. */
        if (strchr(r->path, '$') || strchr(r->path, ':')) {
            p += match[0].rm_eo;
            continue;
        }

        strncpy(r->function_name, name ? name : "", sizeof(r->function_name) - 1);
        strncpy(r->qualified_name, qn ? qn : "", sizeof(r->qualified_name) - 1);
        count++;
        p += match[0].rm_eo;
    }

    cbm_regfree(&laravel_re);
    return count;
}

/* ── Read source lines ─────────────────────────────────────────── */

char *cbm_read_source_lines_disk(const char *root_dir, const char *rel_path, int start_line,
                                 int end_line) {
    char full_path[2048];
    snprintf(full_path, sizeof(full_path), "%s/%s", root_dir, rel_path);

    FILE *f = fopen(full_path, "r");
    if (!f) {
        return NULL;
    }

    char *result = NULL;
    int result_len = 0;
    int result_cap = 0;
    int line = 0;
    char line_buf[4096];

    while (fgets(line_buf, sizeof(line_buf), f)) {
        line++;
        if (line < start_line) {
            continue;
        }
        if (line > end_line) {
            break;
        }

        int llen = (int)strlen(line_buf);
        /* Strip trailing newline */
        if (llen > 0 && line_buf[llen - 1] == '\n') {
            line_buf[--llen] = '\0';
        }

        /* Add separator between lines */
        if (result_len > 0) {
            if (result_len + 1 >= result_cap) {
                result_cap = (result_cap == 0) ? 1024 : result_cap * 2;
                result = safe_realloc(result, (size_t)result_cap);
            }
            result[result_len++] = '\n';
        }

        /* Add line content */
        if (result_len + llen >= result_cap) {
            result_cap = result_len + llen + 256;
            result = safe_realloc(result, (size_t)result_cap);
        }
        memcpy(result + result_len, line_buf, (size_t)llen);
        result_len += llen;
    }

    (void)fclose(f);
    if (result) {
        result[result_len] = '\0';
    }
    return result;
}

/* ── Read source lines from cached buffer ──────────────────────── */

/* Append a line to a dynamically growing result buffer. */
static void append_line_to_result(char **result, int *result_len, int *result_cap, const char *line,
                                  int line_len) {
    /* Strip trailing \r for CRLF files */
    int copy_len = line_len;
    // cppcheck-suppress knownConditionTrueFalse
    if (copy_len > 0 && line[copy_len - 1] == '\r') {
        copy_len--;
    }

    /* Add separator between lines */
    if (*result_len > 0) {
        if (*result_len + 1 >= *result_cap) {
            *result_cap = (*result_cap == 0) ? 1024 : *result_cap * 2;
            *result = safe_realloc(*result, (size_t)*result_cap);
        }
        (*result)[(*result_len)++] = '\n';
    }

    /* Add line content */
    if (*result_len + copy_len >= *result_cap) {
        *result_cap = *result_len + copy_len + 256;
        *result = safe_realloc(*result, (size_t)*result_cap);
    }
    if (copy_len > 0 && *result) {
        memcpy(*result + *result_len, line, (size_t)copy_len);
        *result_len += copy_len;
    }
}

char *cbm_read_source_lines_cached(const char *source, int source_len, int start_line,
                                   int end_line) {
    if (!source || source_len <= 0 || start_line <= 0 || end_line < start_line) {
        return NULL;
    }

    char *result = NULL;
    int result_len = 0;
    int result_cap = 0;
    int line = 0;
    const char *p = source;
    const char *end = source + source_len;

    while (p < end) {
        line++;
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        int llen = eol ? (int)(eol - p) : (int)(end - p);

        if (line >= start_line && line <= end_line) {
            append_line_to_result(&result, &result_len, &result_cap, p, llen);
        }

        if (line > end_line) {
            break;
        }

        p = eol ? eol + 1 : end;
    }

    if (result) {
        result[result_len] = '\0';
    }
    return result;
}

/* ── Config data ───────────────────────────────────────────────── */

const char *cbm_default_exclude_paths[] = {
    "/health",  "/healthz", "/ready",       "/readyz",      "/livez",
    "/metrics", "/debug",   "/favicon.ico", "/.well-known",
};
const int cbm_default_exclude_paths_count = 9;

const char *cbm_http_client_keywords[] = {
    "requests.get", "requests.post", "requests.put", "requests.delete", "httpx.",
    "aiohttp.",     "http.Get",      "http.Post",    "http.NewRequest", "fetch(",
    "axios.",       "HttpClient",    "RestTemplate", "reqwest::",       "hyper::",
    "curl_exec",    "Guzzle",        "sttp.",        "http4s",          "curl_easy",
    "cpr::Get",     "socket.http",   "http.request", "WebClient",       "RestClient",
    "OkHttpClient",
};
const int cbm_http_client_keywords_count = 26;

const char *cbm_async_dispatch_keywords[] = {
    "CreateTask", "create_task", "send_task", "send_message",
    "publish(",   "Publish(",    "SQS",       "EventBridge",
};
const int cbm_async_dispatch_keywords_count = 8;

/* ── Linker config ────────────────────────────────────────────── */

cbm_httplink_config_t cbm_httplink_default_config(void) {
    cbm_httplink_config_t cfg = {0};
    cfg.min_confidence = -1.0; /* sentinel: use default 0.25 */
    cfg.fuzzy_matching = -1;   /* sentinel: use default true */
    cfg.exclude_paths = NULL;
    cfg.exclude_path_count = 0;
    return cfg;
}

cbm_httplink_config_t cbm_httplink_load_config(const char *dir) {
    cbm_httplink_config_t cfg = cbm_httplink_default_config();
    if (!dir) {
        return cfg;
    }

    /* Build path: dir/.cgrconfig */
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/.cgrconfig", dir);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return cfg;
    }

    /* Read file */
    FILE *f = fopen(path, "r");
    if (!f) {
        return cfg;
    }

    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > (long)1024 * 1024) {
        (void)fclose(f);
        return cfg;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        (void)fclose(f);
        return cfg;
    }

    size_t nread = fread(buf, 1, (size_t)size, f);
    (void)fclose(f);
    buf[nread] = '\0';

    /* Parse YAML */
    cbm_yaml_node_t *root = cbm_yaml_parse(buf, (int)nread);
    free(buf);
    if (!root) {
        return cfg;
    }

    /* Extract min_confidence */
    if (cbm_yaml_has(root, "http_linker.min_confidence")) {
        cfg.min_confidence = cbm_yaml_get_float(root, "http_linker.min_confidence", -1.0);
    }

    /* Extract fuzzy_matching */
    if (cbm_yaml_has(root, "http_linker.fuzzy_matching")) {
        cfg.fuzzy_matching = cbm_yaml_get_bool(root, "http_linker.fuzzy_matching", true) ? 1 : 0;
    }

    /* Extract exclude_paths */
    const char *items[128];
    int count = cbm_yaml_get_str_list(root, "http_linker.exclude_paths", items, 128);
    if (count > 0) {
        cfg.exclude_paths = calloc((size_t)count, sizeof(char *));
        if (cfg.exclude_paths) {
            for (int i = 0; i < count; i++) {
                cfg.exclude_paths[i] = strdup(items[i]);
            }
            cfg.exclude_path_count = count;
        }
    }

    cbm_yaml_free(root);
    return cfg;
}

void cbm_httplink_config_free(cbm_httplink_config_t *cfg) {
    if (!cfg) {
        return;
    }
    for (int i = 0; i < cfg->exclude_path_count; i++) {
        free(cfg->exclude_paths[i]);
    }
    free(cfg->exclude_paths);
    cfg->exclude_paths = NULL;
    cfg->exclude_path_count = 0;
}

double cbm_httplink_effective_min_confidence(const cbm_httplink_config_t *cfg) {
    if (!cfg || cfg->min_confidence < 0) {
        return DEFAULT_MIN_CONFIDENCE;
    }
    return cfg->min_confidence;
}

bool cbm_httplink_effective_fuzzy_matching(const cbm_httplink_config_t *cfg) {
    if (!cfg || cfg->fuzzy_matching < 0) {
        return true;
    }
    return cfg->fuzzy_matching == 1;
}

int cbm_httplink_all_exclude_paths(const cbm_httplink_config_t *cfg, const char **out,
                                   int max_out) {
    int count = 0;

    /* Add defaults first */
    for (int i = 0; i < cbm_default_exclude_paths_count && count < max_out; i++) {
        out[count++] = cbm_default_exclude_paths[i];
    }

    /* Add user-configured paths */
    if (cfg) {
        for (int i = 0; i < cfg->exclude_path_count && count < max_out; i++) {
            out[count++] = cfg->exclude_paths[i];
        }
    }

    return count;
}
