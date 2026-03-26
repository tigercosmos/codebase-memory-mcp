#include "cbm.h"
#include "arena.h" // CBMArena, cbm_arena_strndup
#include "helpers.h"
#include "lang_specs.h"
#include "extract_unified.h"
#include "tree_sitter/api.h" // TSNode, ts_node_*
#include <stdint.h>          // uint32_t
#include <string.h>
#include <ctype.h>

// Unquote a string literal: "foo" -> foo, 'foo' -> foo
static const char *unquote(CBMArena *a, const char *s) {
    if (!s || !s[0]) {
        return NULL;
    }
    // Trim whitespace
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t len = strlen(s);
    if (len >= 2) {
        char first = s[0];
        char last = s[len - 1];
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'') ||
            (first == '`' && last == '`')) {
            return cbm_arena_strndup(a, s + 1, len - 2);
        }
    }
    return s;
}

// Extract env key from a function call like os.Getenv("KEY").
static const char *extract_env_key_from_call(CBMExtractCtx *ctx, TSNode node,
                                             const CBMLangSpec *spec) {
    if (!spec->env_access_functions || !spec->env_access_functions[0]) {
        return NULL;
    }

    TSNode func_node = ts_node_child_by_field_name(node, "function", 8);
    if (ts_node_is_null(func_node)) {
        return NULL;
    }
    char *callee = cbm_node_text(ctx->arena, func_node, ctx->source);
    if (!callee) {
        return NULL;
    }

    // Check if callee matches any env access function
    bool match = false;
    for (const char **ef = spec->env_access_functions; *ef; ef++) {
        if (strcmp(callee, *ef) == 0) {
            match = true;
            break;
        }
    }
    if (!match) {
        return NULL;
    }

    // Get first argument (the env key)
    TSNode args = ts_node_child_by_field_name(node, "arguments", 9);
    if (ts_node_is_null(args)) {
        return NULL;
    }
    // Find first named child (skip parentheses)
    for (uint32_t i = 0; i < ts_node_child_count(args); i++) {
        TSNode child = ts_node_child(args, i);
        const char *ck = ts_node_type(child);
        if (strcmp(ck, "(") == 0 || strcmp(ck, ")") == 0 || strcmp(ck, ",") == 0) {
            continue;
        }
        char *arg_text = cbm_node_text(ctx->arena, child, ctx->source);
        return unquote(ctx->arena, arg_text);
    }
    return NULL;
}

// Extract env key from member access like process.env.KEY or os.environ["KEY"].
static const char *extract_env_key_from_member(CBMExtractCtx *ctx, TSNode node,
                                               const CBMLangSpec *spec) {
    if (!spec->env_access_member_patterns || !spec->env_access_member_patterns[0]) {
        return NULL;
    }

    char *text = cbm_node_text(ctx->arena, node, ctx->source);
    if (!text || !text[0]) {
        return NULL;
    }

    for (const char **pat = spec->env_access_member_patterns; *pat; pat++) {
        size_t plen = strlen(*pat);

        // Dot access: pattern.KEY
        if (strncmp(text, *pat, plen) == 0 && text[plen] == '.') {
            const char *key = text + plen + 1;
            // Validate: no further dots/brackets
            if (key[0] && !strchr(key, '.') && !strchr(key, '[')) {
                return key;
            }
        }

        // Subscript: pattern["KEY"]
        if (strncmp(text, *pat, plen) == 0 && text[plen] == '[') {
            const char *inner = text + plen + 1;
            size_t ilen = strlen(inner);
            if (ilen > 0 && inner[ilen - 1] == ']') {
                char *bracket_content = cbm_arena_strndup(ctx->arena, inner, ilen - 1);
                return unquote(ctx->arena, bracket_content);
            }
        }
    }
    return NULL;
}

// Check if an env key name looks like an environment variable (uppercase + underscores).
static bool is_env_var_name(const char *s) {
    if (!s || strlen(s) < 2) {
        return false;
    }
    bool has_upper = false;
    for (const char *p = s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            {
                has_upper = true;
            }
        } else if (*p == '_' || (*p >= '0' && *p <= '9')) { /* ok */
        } else {
            { return false; }
        }
    }
    return has_upper;
}

#define WALK_ENV_MAX_DEPTH 4096

// NOLINTNEXTLINE(misc-no-recursion) — intentional AST tree walk
static void walk_env_inner(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, int depth) {
    if (depth > WALK_ENV_MAX_DEPTH) {
        return;
    }
    const char *kind = ts_node_type(node);
    const char *env_key = NULL;

    // Check call nodes
    if (cbm_kind_in_set(node, spec->call_node_types)) {
        env_key = extract_env_key_from_call(ctx, node, spec);
    }
    // Check member/subscript access
    else if (strcmp(kind, "member_expression") == 0 || strcmp(kind, "subscript") == 0 ||
             strcmp(kind, "attribute") == 0) {
        env_key = extract_env_key_from_member(ctx, node, spec);
    }

    if (env_key && env_key[0] && is_env_var_name(env_key)) {
        CBMEnvAccess ea;
        ea.env_key = env_key;
        ea.enclosing_func_qn = cbm_enclosing_func_qn_cached(ctx, node);
        cbm_envaccess_push(&ctx->result->env_accesses, ctx->arena, ea);
        // Don't recurse into this node's children (avoid double-counting)
        return;
    }

    // Recurse
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; i++) {
        walk_env_inner(ctx, ts_node_child(node, i), spec, depth + 1);
    }
}

static void walk_env_accesses(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec) {
    walk_env_inner(ctx, node, spec, 0);
}

void cbm_extract_env_accesses(CBMExtractCtx *ctx) {
    const CBMLangSpec *spec = cbm_lang_spec(ctx->language);
    if (!spec) {
        return;
    }

    // NOLINTNEXTLINE(readability-implicit-bool-conversion) — pointer-to-bool check for NULL array
    bool has_funcs = spec->env_access_functions && spec->env_access_functions[0];
    // NOLINTNEXTLINE(readability-implicit-bool-conversion) — pointer-to-bool check for NULL array
    bool has_members = spec->env_access_member_patterns && spec->env_access_member_patterns[0];
    if (!has_funcs && !has_members) {
        return;
    }

    walk_env_accesses(ctx, ctx->root, spec);
}

// --- Unified handler ---

void handle_env_accesses(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec,
                         WalkState *state) {
    // NOLINTNEXTLINE(readability-implicit-bool-conversion) — pointer-to-bool check for NULL array
    bool has_funcs = spec->env_access_functions && spec->env_access_functions[0];
    // NOLINTNEXTLINE(readability-implicit-bool-conversion) — pointer-to-bool check for NULL array
    bool has_members = spec->env_access_member_patterns && spec->env_access_member_patterns[0];
    if (!has_funcs && !has_members) {
        return;
    }

    const char *kind = ts_node_type(node);
    const char *env_key = NULL;

    if (has_funcs && spec->call_node_types && cbm_kind_in_set(node, spec->call_node_types)) {
        env_key = extract_env_key_from_call(ctx, node, spec);
    } else if (has_members && (strcmp(kind, "member_expression") == 0 ||
                               strcmp(kind, "subscript") == 0 || strcmp(kind, "attribute") == 0)) {
        env_key = extract_env_key_from_member(ctx, node, spec);
    }

    if (env_key && env_key[0] && is_env_var_name(env_key)) {
        CBMEnvAccess ea;
        ea.env_key = env_key;
        ea.enclosing_func_qn = state->enclosing_func_qn;
        cbm_envaccess_push(&ctx->result->env_accesses, ctx->arena, ea);
    }
}
