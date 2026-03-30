
/*
 * gitignore.c — Gitignore-style pattern matching.
 *
 * Implements the core gitignore pattern matching algorithm:
 *   - * matches anything except /
 *   - ** matches any number of path components
 *   - ? matches any single character except /
 *   - [abc] and [a-z] character classes
 *   - ! prefix for negation
 *   - trailing / for directory-only matching
 *   - patterns with / are rooted (anchored to base)
 */
#include "discover/discover.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Pattern representation ──────────────────────────────────────── */

typedef struct {
    char *pattern; /* the glob pattern (normalized) */
    bool negated;  /* starts with ! */
    bool dir_only; /* ends with / */
    bool rooted;   /* contains / (anchored to root) */
} gi_pattern_t;

struct cbm_gitignore {
    gi_pattern_t *patterns;
    int count;
    int capacity;
};

/* ── Pattern matching engine ─────────────────────────────────────── */

/*
 * Match a glob pattern against a string.
 * Handles: * (non-slash), ** (any path), ? (single non-slash), [class]
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static bool glob_match(const char *pat, const char *str) {
    while (*pat && *str) {
        if (pat[0] == '*' && pat[1] == '*') {
            /* ** — match any number of path components */
            if (pat[2] == '/') {
                /* "** /" at start or middle: try matching rest at every / boundary */
                pat += 3;
                /* Try matching from current position */
                if (glob_match(pat, str)) {
                    return true;
                }
                /* Try matching from every / in str */
                for (const char *s = str; *s; s++) {
                    if (*s == '/' && glob_match(pat, s + 1)) {
                        return true;
                    }
                }
                return false;
            }
            if (pat[2] == '\0') {
                /* ** at end — matches everything */
                return true;
            }
            /* ** followed by non-slash — treat as two *'s, match anything */
            pat += 2;
            for (const char *s = str;; s++) {
                if (glob_match(pat, s)) {
                    return true;
                }
                if (!*s) {
                    return false;
                }
            }
        }

        if (*pat == '*') {
            /* * — match any sequence not containing / */
            pat++;
            /* Try matching rest at every position that doesn't cross / */
            for (const char *s = str;; s++) {
                if (glob_match(pat, s)) {
                    return true;
                }
                if (!*s || *s == '/') {
                    return false;
                }
            }
        }

        if (*pat == '?') {
            /* ? — match any single character except / */
            if (*str == '/') {
                return false;
            }
            pat++;
            str++;
            continue;
        }

        if (*pat == '[') {
            /* Character class */
            pat++;
            bool negate_class = false;
            if (*pat == '!' || *pat == '^') {
                negate_class = true;
                pat++;
            }
            bool matched = false;
            char prev = 0;
            while (*pat && *pat != ']') {
                if (*pat == '-' && prev && pat[1] && pat[1] != ']') {
                    /* Range: [a-z] */
                    pat++;
                    if (*str >= prev && *str <= *pat) {
                        matched = true;
                    }
                    prev = *pat;
                    pat++;
                } else {
                    if (*str == *pat) {
                        matched = true;
                    }
                    prev = *pat;
                    pat++;
                }
            }
            if (*pat == ']') {
                pat++;
            }
            if (negate_class) {
                matched = !matched;
            }
            if (!matched) {
                return false;
            }
            str++;
            continue;
        }

        /* Literal character */
        if (*pat != *str) {
            return false;
        }
        pat++;
        str++;
    }

    /* Handle trailing * or ** in pattern */
    while (*pat == '*') {
        pat++;
    }

    return *pat == '\0' && *str == '\0';
}

/* ── Pattern parsing ─────────────────────────────────────────────── */

static void gi_add_pattern(cbm_gitignore_t *gi, const char *line, int len) {
    /* Trim trailing whitespace */
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r')) {
        len--;
    }
    if (len == 0) {
        return;
    }

    gi_pattern_t p = {0};

    /* Check for negation */
    const char *start = line;
    if (*start == '!') {
        p.negated = true;
        start++;
        len--;
    }

    if (len == 0) {
        return;
    }

    /* Check for trailing / (directory-only) */
    if (start[len - 1] == '/') {
        p.dir_only = true;
        len--;
    }

    if (len == 0) {
        return;
    }

    /* Check for leading / (rooted) */
    if (*start == '/') {
        p.rooted = true;
        start++;
        len--;
    }

    if (len == 0) {
        return;
    }

    /* Check if pattern contains / anywhere (makes it rooted) */
    if (!p.rooted) {
        for (int i = 0; i < len; i++) {
            if (start[i] == '/') {
                p.rooted = true;
                break;
            }
        }
    }

    /* Copy pattern */
    p.pattern = malloc(len + 1);
    if (!p.pattern) {
        return;
    }
    memcpy(p.pattern, start, len);
    p.pattern[len] = '\0';

    /* Grow array if needed */
    if (gi->count >= gi->capacity) {
        int new_cap = gi->capacity ? gi->capacity * 2 : 16;
        gi_pattern_t *new_patterns = realloc(gi->patterns, new_cap * sizeof(gi_pattern_t));
        if (!new_patterns) {
            free(p.pattern);
            return;
        }
        gi->patterns = new_patterns;
        gi->capacity = new_cap;
    }

    gi->patterns[gi->count++] = p;
}

/* ── Public API ──────────────────────────────────────────────────── */

cbm_gitignore_t *cbm_gitignore_parse(const char *content) {
    if (!content) {
        return NULL;
    }

    cbm_gitignore_t *gi = calloc(1, sizeof(cbm_gitignore_t));
    if (!gi) {
        return NULL;
    }

    const char *line = content;
    while (*line) {
        /* Find end of line */
        const char *eol = strchr(line, '\n');
        int len = eol ? (int)(eol - line) : (int)strlen(line);

        /* Skip comments and blank lines */
        if (len > 0 && line[0] != '#') {
            gi_add_pattern(gi, line, len);
        }

        if (!eol) {
            break;
        }
        line = eol + 1;
    }

    return gi;
}

cbm_gitignore_t *cbm_gitignore_load(const char *path) {
    if (!path) {
        return NULL;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }

    /* Read entire file */
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        (void)fclose(f);
        return cbm_gitignore_parse("");
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, size, f);
    buf[n] = '\0';
    (void)fclose(f);

    cbm_gitignore_t *gi = cbm_gitignore_parse(buf);
    free(buf);
    return gi;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool cbm_gitignore_matches(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir) {
    if (!gi || !rel_path) {
        return false;
    }

    /* Extract the basename for non-rooted pattern matching */
    const char *basename = strrchr(rel_path, '/');
    basename = basename ? basename + 1 : rel_path;

    bool matched = false;

    for (int i = 0; i < gi->count; i++) {
        const gi_pattern_t *p = &gi->patterns[i];

        /* Directory-only patterns only match directories */
        if (p->dir_only && !is_dir) {
            continue;
        }

        bool this_match = false;

        if (p->rooted) {
            /* Rooted: match against full relative path */
            this_match = glob_match(p->pattern, rel_path);
        } else {
            /* Non-rooted: match against basename, OR against full path
             * (a non-rooted pattern like "*.log" should match "error.log"
             *  but NOT "logs/error.log" — unless the pattern uses **) */
            this_match = glob_match(p->pattern, basename);

            /* Also try matching against every suffix of the path.
             * This handles "build/" matching "src/build" as a directory. */
            if (!this_match && strchr(rel_path, '/')) {
                /* Try matching at every / boundary */
                const char *s = rel_path;
                while (*s) {
                    if (glob_match(p->pattern, s)) {
                        this_match = true;
                        break;
                    }
                    const char *next = strchr(s, '/');
                    if (!next) {
                        break;
                    }
                    s = next + 1;
                }
            }
        }

        if (this_match) {
            matched = !p->negated;
        }
    }

    return matched;
}

void cbm_gitignore_free(cbm_gitignore_t *gi) {
    if (!gi) {
        return;
    }
    for (int i = 0; i < gi->count; i++) {
        free(gi->patterns[i].pattern);
    }
    free(gi->patterns);
    free(gi);
}
