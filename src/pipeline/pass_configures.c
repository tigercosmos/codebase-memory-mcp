/*
 * pass_configures.c — Config key and env var helpers.
 *
 * Pure helper functions for detecting environment variable names,
 * normalizing config keys, and identifying config file extensions.
 */
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"

#include <ctype.h>
#include <string.h>

bool cbm_is_env_var_name(const char *s) {
    if (!s) {
        return false;
    }
    size_t len = strlen(s);
    if (len < 2) {
        return false;
    }

    bool has_upper = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            has_upper = true;
        } else if (c == '_' || (c >= '0' && c <= '9')) {
            /* ok */
        } else {
            return false;
        }
    }
    return has_upper;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int cbm_normalize_config_key(const char *key, char *norm_out, size_t norm_sz) {
    if (!key || !norm_out || norm_sz == 0) {
        return 0;
    }
    norm_out[0] = '\0';

    /* Split on delimiters: _, -, . */
    /* Then split each part on camelCase transitions */
    /* Collect all words, lowercase them, join with _ */

    char buf[512];
    size_t klen = strlen(key);
    if (klen >= sizeof(buf)) {
        klen = sizeof(buf) - 1;
    }
    memcpy(buf, key, klen);
    buf[klen] = '\0';

    /* First pass: replace delimiters with spaces */
    for (size_t i = 0; i < klen; i++) {
        if (buf[i] == '_' || buf[i] == '-' || buf[i] == '.') {
            buf[i] = ' ';
        }
    }

    /* Extract parts (space-separated) */
    int token_count = 0;
    size_t out_pos = 0;

    char *saveptr = NULL;
    char *part = strtok_r(buf, " ", &saveptr);
    while (part) {
        /* Split camelCase within this part */
        size_t plen = strlen(part);
        size_t start = 0;

        for (size_t i = 1; i < plen; i++) {
            if (part[i] >= 'A' && part[i] <= 'Z' && part[i - 1] >= 'a' && part[i - 1] <= 'z') {
                /* Found transition — emit word */
                if (out_pos > 0 && out_pos < norm_sz - 1) {
                    norm_out[out_pos++] = '_';
                }
                for (size_t j = start; j < i && out_pos < norm_sz - 1; j++) {
                    norm_out[out_pos++] = (char)tolower((unsigned char)part[j]);
                }
                token_count++;
                start = i;
            }
        }
        /* Emit remaining */
        if (out_pos > 0 && out_pos < norm_sz - 1) {
            norm_out[out_pos++] = '_';
        }
        for (size_t j = start; j < plen && out_pos < norm_sz - 1; j++) {
            norm_out[out_pos++] = (char)tolower((unsigned char)part[j]);
        }
        token_count++;

        part = strtok_r(NULL, " ", &saveptr);
    }

    norm_out[out_pos] = '\0';
    return token_count;
}

bool cbm_has_config_extension(const char *path) {
    if (!path) {
        return false;
    }

    /* Find last dot */
    const char *dot = strrchr(path, '.');
    const char *basename = strrchr(path, '/');
    if (!basename) {
        basename = path;
    } else {
        basename++;
    }

    /* Special case: .env files */
    if (strcmp(basename, ".env") == 0) {
        return true;
    }

    if (!dot) {
        return false;
    }

    static const char *exts[] = {".toml", ".ini", ".yaml", ".yml", ".cfg", ".properties",
                                 ".json", ".xml", ".conf", ".env", NULL};

    for (int i = 0; exts[i]; i++) {
        if (strcmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}
