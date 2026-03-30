/*
 * pass_compile_commands.c — compile_commands.json parsing helpers.
 *
 * Parses compile_commands.json to extract per-file include paths, defines,
 * and C/C++ standard flags.
 */
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "yyjson/yyjson.h"

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int cbm_split_command(const char *cmd, char **out, int max_out) {
    if (!cmd || !out || max_out <= 0) {
        return 0;
    }

    int count = 0;
    char current[4096];
    int clen = 0;
    char in_quote = 0;

    for (int i = 0; cmd[i]; i++) {
        char c = cmd[i];
        if (in_quote) {
            if (c == in_quote) {
                in_quote = 0;
            } else {
                if (clen < (int)sizeof(current) - 1) {
                    current[clen++] = c;
                }
            }
        } else if (c == '"' || c == '\'') {
            in_quote = c;
        } else if (c == ' ' || c == '\t') {
            if (clen > 0 && count < max_out) {
                current[clen] = '\0';
                out[count++] = strdup(current);
                clen = 0;
            }
        } else {
            if (clen < (int)sizeof(current) - 1) {
                current[clen++] = c;
            }
        }
    }
    if (clen > 0 && count < max_out) {
        current[clen] = '\0';
        out[count++] = strdup(current);
    }
    return count;
}

/* Resolve a path: if relative, join with directory. */
static char *resolve_path(const char *path, const char *directory) {
    if (!path) {
        return NULL;
    }

    /* Absolute path */
    if (path[0] == '/') {
        return strdup(path);
    }

    /* Relative — join with directory */
    if (directory && directory[0]) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s/%s", directory, path);
        return strdup(buf);
    }

    return strdup(path);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
cbm_compile_flags_t *cbm_extract_flags(const char **args, int argc, const char *directory) {
    cbm_compile_flags_t *f = calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }

    /* Pre-allocate arrays */
    f->include_paths = calloc(argc, sizeof(char *));
    f->defines = calloc(argc, sizeof(char *));

    for (int i = 0; i < argc; i++) {
        const char *arg = args[i];

        /* -I<path> or -I <path> */
        if (arg[0] == '-' && arg[1] == 'I') {
            const char *path = arg + 2;
            if (*path == '\0' && i + 1 < argc) {
                i++;
                path = args[i];
            }
            if (path && *path) {
                f->include_paths[f->include_count++] = resolve_path(path, directory);
            }
            continue;
        }

        /* -isystem <path> */
        if (strcmp(arg, "-isystem") == 0 && i + 1 < argc) {
            i++;
            f->include_paths[f->include_count++] = resolve_path(args[i], directory);
            continue;
        }

        /* -D<name> or -D<name>=<value> */
        if (arg[0] == '-' && arg[1] == 'D') {
            const char *define = arg + 2;
            if (*define == '\0' && i + 1 < argc) {
                i++;
                define = args[i];
            }
            if (define && *define) {
                f->defines[f->define_count++] = strdup(define);
            }
            continue;
        }

        /* -std=<value> */
        if (strncmp(arg, "-std=", 5) == 0) {
            snprintf(f->standard, sizeof(f->standard), "%s", arg + 5);
            continue;
        }
    }

    return f;
}

void cbm_compile_flags_free(cbm_compile_flags_t *f) {
    if (!f) {
        return;
    }
    for (int i = 0; i < f->include_count; i++) {
        free(f->include_paths[i]);
    }
    free(f->include_paths);
    for (int i = 0; i < f->define_count; i++) {
        free(f->defines[i]);
    }
    free(f->defines);
    free(f);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int cbm_parse_compile_commands(const char *json_data, const char *repo_path, char ***out_paths,
                               cbm_compile_flags_t ***out_flags) {
    if (!json_data || !repo_path || !out_paths || !out_flags) {
        return -1;
    }
    *out_paths = NULL;
    *out_flags = NULL;

    yyjson_doc *doc = yyjson_read(json_data, strlen(json_data), 0);
    if (!doc) {
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        return -1;
    }

    int arr_len = (int)yyjson_arr_size(root);
    if (arr_len == 0) {
        yyjson_doc_free(doc);
        return 0;
    }

    char **paths = calloc(arr_len, sizeof(char *));
    cbm_compile_flags_t **flags = calloc(arr_len, sizeof(cbm_compile_flags_t *));
    int count = 0;

    yyjson_val *entry;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(root, &iter);

    while ((entry = yyjson_arr_iter_next(&iter))) {
        yyjson_val *dir_val = yyjson_obj_get(entry, "directory");
        yyjson_val *file_val = yyjson_obj_get(entry, "file");
        yyjson_val *cmd_val = yyjson_obj_get(entry, "command");
        yyjson_val *args_val = yyjson_obj_get(entry, "arguments");

        if (!file_val) {
            continue;
        }
        const char *directory = dir_val ? yyjson_get_str(dir_val) : "";
        const char *file_path = yyjson_get_str(file_val);
        if (!file_path) {
            continue;
        }

        /* Build arguments array */
        char *split_args[256];
        const char *flag_args[256];
        int flag_argc = 0;

        if (args_val && yyjson_is_arr(args_val)) {
            yyjson_val *a;
            yyjson_arr_iter aiter;
            yyjson_arr_iter_init(args_val, &aiter);
            while ((a = yyjson_arr_iter_next(&aiter)) && flag_argc < 256) {
                const char *s = yyjson_get_str(a);
                if (s) {
                    flag_args[flag_argc++] = s;
                }
            }
        } else if (cmd_val && yyjson_is_str(cmd_val)) {
            int n = cbm_split_command(yyjson_get_str(cmd_val), split_args, 256);
            for (int j = 0; j < n; j++) {
                flag_args[j] = split_args[j];
            }
            flag_argc = n;
        }

        if (flag_argc == 0) {
            continue;
        }

        cbm_compile_flags_t *f = cbm_extract_flags(flag_args, flag_argc, directory);

        /* Free split_args if we used splitCommand */
        if (cmd_val && yyjson_is_str(cmd_val)) {
            for (int j = 0; j < flag_argc; j++) {
                free(split_args[j]);
            }
        }

        if (!f) {
            continue;
        }

        /* Compute relative path */
        char abs_path[4096];
        if (file_path[0] != '/' && directory && directory[0]) {
            snprintf(abs_path, sizeof(abs_path), "%s/%s", directory, file_path);
        } else {
            snprintf(abs_path, sizeof(abs_path), "%s", file_path);
        }

        /* Check if inside repo */
        size_t repo_len = strlen(repo_path);
        if (strncmp(abs_path, repo_path, repo_len) != 0 || abs_path[repo_len] != '/') {
            cbm_compile_flags_free(f);
            continue;
        }

        paths[count] = strdup(abs_path + repo_len + 1);
        flags[count] = f;
        count++;
    }

    yyjson_doc_free(doc);
    *out_paths = paths;
    *out_flags = flags;
    return count;
}
