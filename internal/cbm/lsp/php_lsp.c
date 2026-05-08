/*
 * php_lsp.c — PHP Light Semantic Pass.
 *
 * In-process type-aware call resolver for PHP. Mirrors go_lsp.c / c_lsp.c
 * shape:
 *   1. Build a CBMTypeRegistry from file-local definitions + stdlib +
 *      composer PSR-4 mappings (when present).
 *   2. Walk top-level: collect namespace declaration and `use` clauses.
 *   3. Walk each function/method body, push scope, bind typed parameters
 *      and $this, resolve member/static/function call expressions.
 *
 * Scope is the collide-set attribution problem identified in the
 * pre-flight (see docs/PHP_LSP_PRE_FLIGHT.md). Specifically: when a
 * short name (e.g. value) exists as both a global helper function and a
 * method on a Laravel class, $x->value() must route to the method
 * variant whenever the type of $x is statically determinable, and bare
 * value() must route to the helper.
 */

#include "php_lsp.h"
#include "../helpers.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PHP_EVAL_MAX_DEPTH 32
#define PHP_USE_INITIAL_CAP 16

extern const TSLanguage *tree_sitter_php_only(void);

/* Forward decls */
static void php_resolve_calls_in_node(PHPLSPContext *ctx, TSNode node);
static void process_function_like(PHPLSPContext *ctx, TSNode node);
static void process_class_decl(PHPLSPContext *ctx, TSNode node);
static const CBMType *eval_function_call_type(PHPLSPContext *ctx, TSNode call_node);
static const CBMType *eval_member_call_type(PHPLSPContext *ctx, TSNode call_node);
static const CBMType *eval_object_creation_type(PHPLSPContext *ctx, TSNode node);
static void bind_phpdoc_var(PHPLSPContext *ctx, const char *docstring);
static void parse_phpdoc_for_params(PHPLSPContext *ctx, const char *docstring, TSNode params);
static const CBMType *resolve_phpdoc_type(PHPLSPContext *ctx, const char *type_text);
static char *fetch_leading_phpdoc(PHPLSPContext *ctx, TSNode node);

/* ── helpers ────────────────────────────────────────────────────── */

static char *php_node_text(PHPLSPContext *ctx, TSNode node) {
    return cbm_node_text(ctx->arena, node, ctx->source);
}

static bool node_is(TSNode n, const char *kind) {
    if (ts_node_is_null(n)) return false;
    return strcmp(ts_node_type(n), kind) == 0;
}

static TSNode child_named(TSNode parent, const char *kind) {
    if (ts_node_is_null(parent)) return parent;
    uint32_t nc = ts_node_child_count(parent);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(parent, i);
        if (!ts_node_is_null(c) && strcmp(ts_node_type(c), kind) == 0) return c;
    }
    TSNode null_node;
    memset(&null_node, 0, sizeof(null_node));
    return null_node;
}

/* PHP qualified names use "." in the graph (project.path.module.class[.method]).
 * Convert "App\\Models\\User" to "App.Models.User" so we can compose with
 * module_qn (which already uses ".") and look up registry entries. */
static char *php_ns_to_dot(CBMArena *a, const char *ns) {
    if (!ns) return NULL;
    size_t n = strlen(ns);
    char *out = (char *)cbm_arena_alloc(a, n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        char c = ns[i];
        out[i] = (c == '\\') ? '.' : c;
    }
    out[n] = '\0';
    /* Trim leading dot from "\\Foo". */
    if (out[0] == '.') return out + 1;
    return out;
}

/* Return the substring after the last '.' or '\\'. */
static const char *php_short_name(const char *qn) {
    if (!qn) return NULL;
    const char *last = qn;
    for (const char *p = qn; *p; p++) {
        if (*p == '.' || *p == '\\') last = p + 1;
    }
    return last;
}

static bool php_is_builtin_type_name(const char *n) {
    if (!n) return false;
    static const char *const builtins[] = {"int",      "integer", "float",   "double",
                                           "string",   "bool",    "boolean", "array",
                                           "callable", "iterable", "object", "void",
                                           "mixed",    "never",   "null",    "true",
                                           "false",    "self",    "static",  "parent",
                                           NULL};
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(n, builtins[i]) == 0) return true;
    }
    return false;
}

/* ── init / context ─────────────────────────────────────────────── */

void php_lsp_init(PHPLSPContext *ctx, CBMArena *arena, const char *source, int source_len,
                  const CBMTypeRegistry *registry, const char *module_qn,
                  CBMResolvedCallArray *out) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->arena = arena;
    ctx->source = source;
    ctx->source_len = source_len;
    ctx->registry = registry;
    ctx->module_qn = module_qn;
    ctx->current_namespace_qn = "";
    ctx->resolved_calls = out;
    ctx->current_scope = cbm_scope_push(arena, NULL);

    const char *dbg = getenv("CBM_LSP_DEBUG");
    ctx->debug = (dbg && dbg[0]);
}

void php_lsp_add_use(PHPLSPContext *ctx, const char *local_name, const char *target_qn,
                     int use_kind) {
    if (!local_name || !target_qn) return;
    if (ctx->use_count >= ctx->use_cap) {
        int new_cap = ctx->use_cap ? ctx->use_cap * 2 : PHP_USE_INITIAL_CAP;
        const char **nl =
            (const char **)cbm_arena_alloc(ctx->arena, (size_t)new_cap * sizeof(*nl));
        const char **nq =
            (const char **)cbm_arena_alloc(ctx->arena, (size_t)new_cap * sizeof(*nq));
        int *nk = (int *)cbm_arena_alloc(ctx->arena, (size_t)new_cap * sizeof(int));
        if (!nl || !nq || !nk) return;
        for (int i = 0; i < ctx->use_count; i++) {
            nl[i] = ctx->use_local_names[i];
            nq[i] = ctx->use_target_qns[i];
            nk[i] = (int)ctx->use_kinds[i];
        }
        ctx->use_local_names = nl;
        ctx->use_target_qns = nq;
        /* re-cast to the enum-typed pointer */
        ctx->use_kinds = (void *)nk;
        ctx->use_cap = new_cap;
    }
    ctx->use_local_names[ctx->use_count] = cbm_arena_strdup(ctx->arena, local_name);
    ctx->use_target_qns[ctx->use_count] = cbm_arena_strdup(ctx->arena, target_qn);
    ((int *)ctx->use_kinds)[ctx->use_count] = use_kind;
    ctx->use_count++;
}

static const char *find_use(PHPLSPContext *ctx, const char *local_name, int kind) {
    for (int i = 0; i < ctx->use_count; i++) {
        if ((int)ctx->use_kinds[i] != kind) continue;
        if (strcmp(ctx->use_local_names[i], local_name) == 0) return ctx->use_target_qns[i];
    }
    return NULL;
}

/* Resolve a class identifier to a fully-qualified registry key.
 *   - Fully-qualified ("\\Foo\\Bar"):       FOO.BAR — strip leading slash.
 *   - Aliased / single segment matches a use:  the use target.
 *   - First segment matches a use:               substitute and append remainder.
 *   - Otherwise:                                 prefix with current_namespace_qn (or module).
 *
 * Returns an arena-allocated string in dotted form. May return NULL for builtins. */
const char *php_resolve_class_name(PHPLSPContext *ctx, const char *name) {
    if (!name || !*name) return NULL;
    if (php_is_builtin_type_name(name)) return NULL;

    /* Self / static / parent are resolved by caller; still translate here for
     * cases where they slip through. */
    if (strcmp(name, "self") == 0 || strcmp(name, "static") == 0) {
        return ctx->enclosing_class_qn;
    }
    if (strcmp(name, "parent") == 0) {
        return ctx->enclosing_parent_qn;
    }

    /* Fully-qualified — leading backslash. */
    if (name[0] == '\\') {
        return php_ns_to_dot(ctx->arena, name + 1);
    }

    /* Split first segment. */
    const char *sep = name;
    while (*sep && *sep != '\\') sep++;
    char *first;
    if (*sep) {
        first = cbm_arena_strndup(ctx->arena, name, (size_t)(sep - name));
    } else {
        first = cbm_arena_strdup(ctx->arena, name);
    }

    const char *use_target = find_use(ctx, first, CBM_PHP_USE_CLASS);
    if (use_target) {
        if (*sep) {
            /* use App\\Models as M;  M\\User -> App.Models.User */
            return cbm_arena_sprintf(ctx->arena, "%s%s",
                                     use_target,
                                     php_ns_to_dot(ctx->arena, sep));
        }
        return use_target;
    }

    /* Fallback for a same-file class reference: prefer module_qn-prefixed QN
     * because that's what the unified extractor records. The PHP namespace
     * declaration is *not* incorporated into the unified extractor's QNs
     * (defs are keyed by file path + class name), so building from
     * current_namespace_qn here would diverge and miss every same-file
     * lookup. */
    if (ctx->module_qn && ctx->module_qn[0]) {
        return cbm_arena_sprintf(ctx->arena, "%s.%s", ctx->module_qn,
                                 php_ns_to_dot(ctx->arena, name));
    }
    return php_ns_to_dot(ctx->arena, name);
}

/* Try to find a registered type for a "namespaced" QN.
 *
 * Lookup order:
 *   1. exact QN match
 *   2. module_qn + "." + qn (same-file class)
 *   3. project_root + "." + qn (drop trailing module segments one at a time)
 *   4. fall back to a short-name scan of the registry, preferring the
 *      candidate whose QN shares the longest dot-prefix with module_qn
 *      (cross-file class in the same project tree).
 *
 * Step 4 is critical for PHP because the unified extractor builds QNs from
 * file paths but `use App\\Models\\User` produces an "App.Models.User" key.
 * Without short-name fallback, every cross-file class resolution would miss. */
static const CBMRegisteredType *lookup_type_with_project(PHPLSPContext *ctx, const char *qn) {
    if (!qn) return NULL;
    const CBMRegisteredType *t = cbm_registry_lookup_type(ctx->registry, qn);
    if (t) return t;

    if (ctx->module_qn && ctx->module_qn[0]) {
        const char *combo = cbm_arena_sprintf(ctx->arena, "%s.%s", ctx->module_qn, qn);
        t = cbm_registry_lookup_type(ctx->registry, combo);
        if (t) return t;

        const char *m = ctx->module_qn;
        const char *last_dot = strrchr(m, '.');
        while (last_dot) {
            char *prefix = cbm_arena_strndup(ctx->arena, m, (size_t)(last_dot - m));
            const char *try_qn = cbm_arena_sprintf(ctx->arena, "%s.%s", prefix, qn);
            t = cbm_registry_lookup_type(ctx->registry, try_qn);
            if (t) return t;
            const char *prev = last_dot;
            last_dot = NULL;
            for (const char *p = m; p < prev; p++) {
                if (*p == '.') last_dot = p;
            }
        }
    }

    /* Step 4: short-name fallback. */
    const char *short_name = qn;
    for (const char *p = qn; *p; p++) {
        if (*p == '.') short_name = p + 1;
    }
    if (!short_name || !*short_name) return NULL;

    const CBMRegisteredType *best = NULL;
    int best_score = -1;
    for (int i = 0; ctx->registry && i < ctx->registry->type_count; i++) {
        const CBMRegisteredType *cand = &ctx->registry->types[i];
        if (!cand->short_name || strcmp(cand->short_name, short_name) != 0) continue;
        int score = 0;
        if (cand->qualified_name && ctx->module_qn) {
            const char *m = ctx->module_qn;
            const char *q = cand->qualified_name;
            while (*m && *q && *m == *q) {
                if (*m == '.') score++;
                m++;
                q++;
            }
        }
        if (score > best_score) {
            best_score = score;
            best = cand;
        }
    }
    return best;
}

/* ── method lookup with parent walk ─────────────────────────────── */

const CBMRegisteredFunc *php_lookup_method(PHPLSPContext *ctx, const char *class_qn,
                                           const char *method_name) {
    if (!class_qn || !method_name) return NULL;

    /* Direct lookup by the registry's receiver_qn -> method_name index. */
    const CBMRegisteredFunc *f = cbm_registry_lookup_method(ctx->registry, class_qn, method_name);
    if (f) return f;

    /* If the QN we have is not what the registry uses (e.g. caller built it
     * from PHP namespace, but the unified extractor keyed by file path),
     * resolve the type to its registered identity and retry. */
    const CBMRegisteredType *t = cbm_registry_lookup_type(ctx->registry, class_qn);
    if (!t) t = lookup_type_with_project(ctx, class_qn);
    if (!t) return NULL;
    if (strcmp(t->qualified_name, class_qn) != 0) {
        f = cbm_registry_lookup_method(ctx->registry, t->qualified_name, method_name);
        if (f) return f;
    }

    /* Bounded chain walk. */
    int hops = 0;
    while (t && hops < 16) {
        if (t->method_qns) {
            for (int i = 0; t->method_names && t->method_names[i]; i++) {
                if (strcmp(t->method_names[i], method_name) == 0) {
                    return cbm_registry_lookup_func(ctx->registry, t->method_qns[i]);
                }
            }
        }
        /* Try first parent. */
        const char *parent = NULL;
        if (t->embedded_types && t->embedded_types[0]) parent = t->embedded_types[0];
        if (!parent) break;
        f = cbm_registry_lookup_method(ctx->registry, parent, method_name);
        if (f) return f;
        const CBMRegisteredType *next = cbm_registry_lookup_type(ctx->registry, parent);
        if (!next) next = lookup_type_with_project(ctx, parent);
        if (!next) break;
        t = next;
        hops++;
    }
    return NULL;
}

/* Detect a __call / __callStatic on a class chain. */
static bool class_has_magic_call(PHPLSPContext *ctx, const char *class_qn, bool is_static) {
    const char *magic = is_static ? "__callStatic" : "__call";
    return php_lookup_method(ctx, class_qn, magic) != NULL;
}

/* ── type parsing ───────────────────────────────────────────────── */

const CBMType *php_parse_type_node(PHPLSPContext *ctx, TSNode node) {
    if (ts_node_is_null(node)) return cbm_type_unknown();

    const char *kind = ts_node_type(node);

    if (strcmp(kind, "primitive_type") == 0) {
        char *t = php_node_text(ctx, node);
        return cbm_type_builtin(ctx->arena, t ? t : "mixed");
    }
    if (strcmp(kind, "named_type") == 0 || strcmp(kind, "qualified_name") == 0 ||
        strcmp(kind, "name") == 0) {
        char *t = php_node_text(ctx, node);
        if (!t) return cbm_type_unknown();
        const char *resolved = php_resolve_class_name(ctx, t);
        if (!resolved) return cbm_type_unknown();
        return cbm_type_named(ctx->arena, resolved);
    }
    if (strcmp(kind, "optional_type") == 0) {
        /* ?Foo — strip the '?' and recurse. */
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode c = ts_node_child(node, i);
            if (!ts_node_is_null(c) && ts_node_is_named(c)) {
                return php_parse_type_node(ctx, c);
            }
        }
        return cbm_type_unknown();
    }
    if (strcmp(kind, "union_type") == 0 || strcmp(kind, "intersection_type") == 0 ||
        strcmp(kind, "disjunctive_normal_form_type") == 0) {
        /* Pick the first concrete (non-null) named member as a heuristic;
         * full DNF dispatch is Phase 2.5 at best. */
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode c = ts_node_child(node, i);
            if (ts_node_is_null(c) || !ts_node_is_named(c)) continue;
            const CBMType *t = php_parse_type_node(ctx, c);
            if (t && t->kind != CBM_TYPE_UNKNOWN) {
                if (t->kind == CBM_TYPE_BUILTIN && strcmp(t->data.builtin.name, "null") == 0) {
                    continue;
                }
                return t;
            }
        }
        return cbm_type_unknown();
    }
    /* Fallback: try to read raw text and treat as a name. */
    char *t = php_node_text(ctx, node);
    if (!t) return cbm_type_unknown();
    if (php_is_builtin_type_name(t)) return cbm_type_builtin(ctx->arena, t);
    const char *resolved = php_resolve_class_name(ctx, t);
    if (!resolved) return cbm_type_unknown();
    return cbm_type_named(ctx->arena, resolved);
}

/* ── PHPDoc minimal parser ──────────────────────────────────────── */

/* Strip leading "/**", trailing "*​/", and per-line "*" prefixes. Returns a
 * mutable arena-allocated cleaned copy. */
static char *phpdoc_clean(CBMArena *a, const char *raw) {
    if (!raw) return NULL;
    size_t n = strlen(raw);
    char *out = (char *)cbm_arena_alloc(a, n + 1);
    if (!out) return NULL;
    size_t w = 0;
    size_t i = 0;
    /* Skip leading /* and ** */
    while (i < n && (raw[i] == '/' || raw[i] == '*' || raw[i] == ' ' || raw[i] == '\t')) i++;
    bool at_line_start = false;
    for (; i < n; i++) {
        char c = raw[i];
        if (c == '\n') {
            out[w++] = ' ';
            at_line_start = true;
            continue;
        }
        if (at_line_start) {
            if (c == ' ' || c == '\t' || c == '*') continue;
            at_line_start = false;
        }
        out[w++] = c;
    }
    /* Trim trailing */ if (w >= 2 && out[w - 1] == '/' && out[w - 2] == '*') w -= 2;
    out[w] = '\0';
    return out;
}

/* (next_tag helper not currently needed — phpdoc parsers use strstr inline.) */

/* Resolve a PHPDoc-style type spec (e.g. "App\\Foo|null", "?App\\Foo",
 * "Collection<User>", "string") to a CBMType. We strip nullables, take the
 * leftmost class in unions, drop generic <...> tails, then resolve. */
static const CBMType *resolve_phpdoc_type(PHPLSPContext *ctx, const char *type_text) {
    if (!type_text || !*type_text) return cbm_type_unknown();
    /* Skip whitespace */
    while (*type_text == ' ' || *type_text == '\t' || *type_text == '?') type_text++;
    /* Take portion up to first '|', ' ', '\t' */
    size_t end = 0;
    while (type_text[end] && type_text[end] != '|' && type_text[end] != ' ' &&
           type_text[end] != '\t' && type_text[end] != '<' && type_text[end] != '\n') {
        end++;
    }
    if (end == 0) return cbm_type_unknown();
    char *first = cbm_arena_strndup(ctx->arena, type_text, end);
    if (!first) return cbm_type_unknown();
    if (strcmp(first, "null") == 0) {
        /* take next member */
        if (type_text[end] == '|') return resolve_phpdoc_type(ctx, type_text + end + 1);
        return cbm_type_unknown();
    }
    if (php_is_builtin_type_name(first)) return cbm_type_builtin(ctx->arena, first);
    const char *resolved = php_resolve_class_name(ctx, first);
    if (!resolved) return cbm_type_unknown();
    return cbm_type_named(ctx->arena, resolved);
}

/* @param Type $name — rebind matching parameter. */
static void parse_phpdoc_for_params(PHPLSPContext *ctx, const char *docstring, TSNode params) {
    (void)params;
    if (!docstring || !ctx->current_scope) return;
    const char *p = docstring;
    while ((p = strstr(p, "@param")) != NULL) {
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        /* type until whitespace */
        const char *type_start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (p == type_start) continue;
        char *type_text = cbm_arena_strndup(ctx->arena, type_start, (size_t)(p - type_start));
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '$') continue;
        p++;
        const char *name_start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        if (p == name_start) continue;
        char *vname = cbm_arena_strndup(ctx->arena, name_start, (size_t)(p - name_start));
        const CBMType *t = resolve_phpdoc_type(ctx, type_text);
        if (vname && t && t->kind != CBM_TYPE_UNKNOWN) {
            cbm_scope_bind(ctx->current_scope, vname, t);
        }
    }
}

/* @var Type $name — bind a variable in current scope. */
static void bind_phpdoc_var(PHPLSPContext *ctx, const char *docstring) {
    if (!docstring || !ctx->current_scope) return;
    const char *p = docstring;
    while ((p = strstr(p, "@var")) != NULL) {
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        const char *type_start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (p == type_start) continue;
        char *type_text = cbm_arena_strndup(ctx->arena, type_start, (size_t)(p - type_start));
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '$') continue;
        p++;
        const char *name_start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        if (p == name_start) continue;
        char *vname = cbm_arena_strndup(ctx->arena, name_start, (size_t)(p - name_start));
        const CBMType *t = resolve_phpdoc_type(ctx, type_text);
        if (vname && t && t->kind != CBM_TYPE_UNKNOWN) {
            cbm_scope_bind(ctx->current_scope, vname, t);
        }
    }
}

/* Walk siblings backwards from `node` to find a leading PHPDoc comment
 * ("/**...*​/"). Returns cleaned doc text or NULL. */
static char *fetch_leading_phpdoc(PHPLSPContext *ctx, TSNode node) {
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent)) return NULL;
    uint32_t nc = ts_node_child_count(parent);
    /* Find index of node within parent. */
    int idx = -1;
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(parent, i);
        if (ts_node_eq(c, node)) {
            idx = (int)i;
            break;
        }
    }
    if (idx <= 0) return NULL;
    for (int i = idx - 1; i >= 0; i--) {
        TSNode c = ts_node_child(parent, (uint32_t)i);
        if (ts_node_is_null(c)) continue;
        const char *k = ts_node_type(c);
        if (strcmp(k, "comment") == 0) {
            char *raw = php_node_text(ctx, c);
            if (raw && raw[0] == '/' && raw[1] == '*' && raw[2] == '*') {
                return phpdoc_clean(ctx->arena, raw);
            }
            return NULL;
        }
        /* Skip whitespace-only or attributes; stop at any other node. */
        if (strcmp(k, "attribute_list") == 0 || strcmp(k, "attribute_group") == 0) continue;
        return NULL;
    }
    return NULL;
}

/* ── expression evaluation ──────────────────────────────────────── */

/* Returns the type of a tree-sitter PHP expression node.
 * Coverage:
 *   - variable_name ($x):                          scope lookup ($this special-cased).
 *   - object_creation_expression (new X):          named X.
 *   - member_call_expression / nullsafe variant:   recurse into return type.
 *   - scoped_call_expression (X::m()):             recurse into return type.
 *   - function_call_expression:                    function return type.
 *   - assignment_expression ($a = ...):            type of RHS.
 *   - cast_expression:                             casted-to type.
 *   - parenthesized_expression:                    inner expression.
 *   - encapsed_string / string:                    builtin "string".
 *   - integer / float:                             builtin "int"/"float".
 *   - boolean / true / false:                      builtin "bool".
 *   - null:                                        builtin "null".
 *   - clone_expression:                            type of operand.
 */
const CBMType *php_eval_expr_type(PHPLSPContext *ctx, TSNode node) {
    if (ts_node_is_null(node) || ctx->eval_depth >= PHP_EVAL_MAX_DEPTH) {
        return cbm_type_unknown();
    }
    ctx->eval_depth++;
    const CBMType *result = cbm_type_unknown();
    const char *kind = ts_node_type(node);

    if (strcmp(kind, "variable_name") == 0) {
        char *t = php_node_text(ctx, node);
        if (t) {
            /* Strip leading $. */
            const char *name = (t[0] == '$') ? t + 1 : t;
            if (strcmp(name, "this") == 0) {
                if (ctx->enclosing_class_qn) {
                    result = cbm_type_named(ctx->arena, ctx->enclosing_class_qn);
                }
            } else {
                const CBMType *bound = cbm_scope_lookup(ctx->current_scope, name);
                if (bound) result = bound;
            }
        }
    } else if (strcmp(kind, "object_creation_expression") == 0) {
        result = eval_object_creation_type(ctx, node);
    } else if (strcmp(kind, "member_call_expression") == 0 ||
               strcmp(kind, "nullsafe_member_call_expression") == 0 ||
               strcmp(kind, "scoped_call_expression") == 0) {
        result = eval_member_call_type(ctx, node);
    } else if (strcmp(kind, "function_call_expression") == 0) {
        result = eval_function_call_type(ctx, node);
    } else if (strcmp(kind, "assignment_expression") == 0) {
        TSNode rhs = ts_node_child_by_field_name(node, "right", 5);
        if (!ts_node_is_null(rhs)) result = php_eval_expr_type(ctx, rhs);
    } else if (strcmp(kind, "cast_expression") == 0) {
        TSNode tnode = ts_node_child_by_field_name(node, "type", 4);
        if (!ts_node_is_null(tnode)) result = php_parse_type_node(ctx, tnode);
    } else if (strcmp(kind, "parenthesized_expression") == 0) {
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode c = ts_node_child(node, i);
            if (!ts_node_is_null(c) && ts_node_is_named(c)) {
                result = php_eval_expr_type(ctx, c);
                break;
            }
        }
    } else if (strcmp(kind, "clone_expression") == 0) {
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode c = ts_node_child(node, i);
            if (!ts_node_is_null(c) && ts_node_is_named(c)) {
                result = php_eval_expr_type(ctx, c);
                break;
            }
        }
    } else if (strcmp(kind, "encapsed_string") == 0 || strcmp(kind, "string") == 0 ||
               strcmp(kind, "string_value") == 0 || strcmp(kind, "heredoc") == 0) {
        result = cbm_type_builtin(ctx->arena, "string");
    } else if (strcmp(kind, "integer") == 0) {
        result = cbm_type_builtin(ctx->arena, "int");
    } else if (strcmp(kind, "float") == 0) {
        result = cbm_type_builtin(ctx->arena, "float");
    } else if (strcmp(kind, "boolean") == 0 || strcmp(kind, "true") == 0 ||
               strcmp(kind, "false") == 0) {
        result = cbm_type_builtin(ctx->arena, "bool");
    } else if (strcmp(kind, "null") == 0) {
        result = cbm_type_builtin(ctx->arena, "null");
    } else if (strcmp(kind, "member_access_expression") == 0 ||
               strcmp(kind, "nullsafe_member_access_expression") == 0) {
        /* $x->prop — look up field type if known. */
        TSNode obj = ts_node_child_by_field_name(node, "object", 6);
        TSNode field = ts_node_child_by_field_name(node, "name", 4);
        if (!ts_node_is_null(obj) && !ts_node_is_null(field)) {
            const CBMType *recv = php_eval_expr_type(ctx, obj);
            if (recv && recv->kind == CBM_TYPE_NAMED) {
                char *fname = php_node_text(ctx, field);
                const CBMRegisteredType *t = cbm_registry_lookup_type(ctx->registry,
                                                                     recv->data.named.qualified_name);
                if (!t) t = lookup_type_with_project(ctx, recv->data.named.qualified_name);
                if (t && t->field_names && fname) {
                    for (int i = 0; t->field_names[i]; i++) {
                        if (strcmp(t->field_names[i], fname) == 0) {
                            if (t->field_types && t->field_types[i]) {
                                result = t->field_types[i];
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    ctx->eval_depth--;
    return result ? result : cbm_type_unknown();
}

static const CBMType *eval_object_creation_type(PHPLSPContext *ctx, TSNode node) {
    /* Find the class identifier in `new X(...)`. */
    TSNode name_node;
    memset(&name_node, 0, sizeof(name_node));
    name_node = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(name_node)) {
        /* Fallback: first named child that is a name/qualified_name. */
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode c = ts_node_child(node, i);
            if (ts_node_is_null(c) || !ts_node_is_named(c)) continue;
            const char *k = ts_node_type(c);
            if (strcmp(k, "name") == 0 || strcmp(k, "qualified_name") == 0) {
                name_node = c;
                break;
            }
        }
    }
    if (ts_node_is_null(name_node)) return cbm_type_unknown();
    char *name = php_node_text(ctx, name_node);
    if (!name) return cbm_type_unknown();
    const char *resolved = php_resolve_class_name(ctx, name);
    if (!resolved) return cbm_type_unknown();
    return cbm_type_named(ctx->arena, resolved);
}

static const CBMType *eval_function_call_type(PHPLSPContext *ctx, TSNode call_node) {
    TSNode fn = ts_node_child_by_field_name(call_node, "function", 8);
    if (ts_node_is_null(fn)) return cbm_type_unknown();
    char *name = php_node_text(ctx, fn);
    if (!name) return cbm_type_unknown();
    /* Try `use function` map first. */
    const char *target = find_use(ctx, name, CBM_PHP_USE_FUNCTION);
    const char *qn = NULL;
    if (target) {
        qn = target;
    } else if (ctx->current_namespace_qn && ctx->current_namespace_qn[0]) {
        qn = cbm_arena_sprintf(ctx->arena, "%s.%s", ctx->current_namespace_qn, name);
    } else {
        qn = cbm_arena_strdup(ctx->arena, name);
    }
    const CBMRegisteredFunc *f = cbm_registry_lookup_func(ctx->registry, qn);
    if (!f && ctx->module_qn) {
        f = cbm_registry_lookup_func(ctx->registry,
                                     cbm_arena_sprintf(ctx->arena, "%s.%s", ctx->module_qn, name));
    }
    if (!f && ctx->registry) {
        /* Walk the symbol table by short_name as a last resort. */
        for (int i = 0; i < ctx->registry->func_count; i++) {
            const CBMRegisteredFunc *cand = &ctx->registry->funcs[i];
            if (cand->receiver_type) continue;
            if (cand->short_name && strcmp(cand->short_name, name) == 0) {
                f = cand;
                break;
            }
        }
    }
    if (!f || !f->signature) return cbm_type_unknown();
    /* PHP single-return convention. */
    const CBMType *sig = f->signature;
    if (sig->kind == CBM_TYPE_FUNC && sig->data.func.return_types &&
        sig->data.func.return_types[0]) {
        return sig->data.func.return_types[0];
    }
    return cbm_type_unknown();
}

/* Returns the return type of an instance/static method call expression. */
static const CBMType *eval_member_call_type(PHPLSPContext *ctx, TSNode call_node) {
    const char *kind = ts_node_type(call_node);
    bool is_static = strcmp(kind, "scoped_call_expression") == 0;

    TSNode recv_node;
    TSNode name_node;
    if (is_static) {
        recv_node = ts_node_child_by_field_name(call_node, "scope", 5);
        name_node = ts_node_child_by_field_name(call_node, "name", 4);
    } else {
        recv_node = ts_node_child_by_field_name(call_node, "object", 6);
        name_node = ts_node_child_by_field_name(call_node, "name", 4);
    }
    if (ts_node_is_null(name_node)) return cbm_type_unknown();
    char *method_name = php_node_text(ctx, name_node);
    if (!method_name) return cbm_type_unknown();

    const char *class_qn = NULL;
    if (is_static) {
        if (!ts_node_is_null(recv_node)) {
            char *cls = php_node_text(ctx, recv_node);
            if (cls) {
                if (strcmp(cls, "self") == 0 || strcmp(cls, "static") == 0)
                    class_qn = ctx->enclosing_class_qn;
                else if (strcmp(cls, "parent") == 0)
                    class_qn = ctx->enclosing_parent_qn;
                else
                    class_qn = php_resolve_class_name(ctx, cls);
            }
        }
    } else {
        const CBMType *recv_type = ts_node_is_null(recv_node)
                                       ? cbm_type_unknown()
                                       : php_eval_expr_type(ctx, recv_node);
        if (recv_type && recv_type->kind == CBM_TYPE_NAMED) {
            class_qn = recv_type->data.named.qualified_name;
        }
    }
    if (!class_qn) return cbm_type_unknown();

    const CBMRegisteredFunc *f = php_lookup_method(ctx, class_qn, method_name);
    if (!f || !f->signature) return cbm_type_unknown();
    const CBMType *sig = f->signature;
    if (sig->kind == CBM_TYPE_FUNC && sig->data.func.return_types &&
        sig->data.func.return_types[0]) {
        return sig->data.func.return_types[0];
    }
    return cbm_type_unknown();
}

/* ── emit ───────────────────────────────────────────────────────── */

static void emit_resolved(PHPLSPContext *ctx, const char *callee_qn, const char *strategy,
                          float confidence) {
    if (!ctx->resolved_calls || !callee_qn || !ctx->enclosing_func_qn) return;
    CBMResolvedCall rc;
    rc.caller_qn = ctx->enclosing_func_qn;
    rc.callee_qn = callee_qn;
    rc.strategy = strategy;
    rc.confidence = confidence;
    rc.reason = NULL;
    cbm_resolvedcall_push(ctx->resolved_calls, ctx->arena, rc);
}

static void emit_unresolved(PHPLSPContext *ctx, const char *expr_text, const char *reason) {
    if (!ctx->resolved_calls || !ctx->enclosing_func_qn) return;
    CBMResolvedCall rc;
    rc.caller_qn = ctx->enclosing_func_qn;
    rc.callee_qn = expr_text ? expr_text : "?";
    rc.strategy = "lsp_unresolved";
    rc.confidence = 0.0f;
    rc.reason = reason;
    cbm_resolvedcall_push(ctx->resolved_calls, ctx->arena, rc);
}

/* ── statement-level scope binding ──────────────────────────────── */

/* Recognise `$x = new Foo(...)` / `$x = Foo::create()` etc. and bind the LHS. */
static void process_assignment(PHPLSPContext *ctx, TSNode node) {
    TSNode lhs = ts_node_child_by_field_name(node, "left", 4);
    TSNode rhs = ts_node_child_by_field_name(node, "right", 5);
    if (ts_node_is_null(lhs) || ts_node_is_null(rhs)) return;
    if (!node_is(lhs, "variable_name")) return;
    char *t = php_node_text(ctx, lhs);
    if (!t) return;
    const char *name = (t[0] == '$') ? t + 1 : t;
    if (strcmp(name, "this") == 0) return;
    const CBMType *rhs_type = php_eval_expr_type(ctx, rhs);
    if (rhs_type && rhs_type->kind != CBM_TYPE_UNKNOWN) {
        cbm_scope_bind(ctx->current_scope, name, rhs_type);
    }
}

static void process_foreach(PHPLSPContext *ctx, TSNode node) {
    /* Bind iteration variable to "mixed" — we don't track collection<T> yet. */
    uint32_t nc = ts_node_child_count(node);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(node, i);
        if (ts_node_is_null(c) || !ts_node_is_named(c)) continue;
        if (node_is(c, "variable_name")) {
            char *t = php_node_text(ctx, c);
            if (!t) continue;
            const char *name = (t[0] == '$') ? t + 1 : t;
            cbm_scope_bind(ctx->current_scope, name, cbm_type_unknown());
        }
    }
}

static void process_catch(PHPLSPContext *ctx, TSNode node) {
    /* Bind $e to the caught exception type. */
    TSNode type_list;
    memset(&type_list, 0, sizeof(type_list));
    TSNode var_node;
    memset(&var_node, 0, sizeof(var_node));
    uint32_t nc = ts_node_child_count(node);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(node, i);
        if (ts_node_is_null(c) || !ts_node_is_named(c)) continue;
        const char *k = ts_node_type(c);
        if (strcmp(k, "type_list") == 0) type_list = c;
        if (strcmp(k, "variable_name") == 0) var_node = c;
    }
    if (ts_node_is_null(var_node)) return;
    char *vt = php_node_text(ctx, var_node);
    if (!vt) return;
    const char *vname = (vt[0] == '$') ? vt + 1 : vt;
    const CBMType *exc_type = cbm_type_unknown();
    if (!ts_node_is_null(type_list)) {
        uint32_t tnc = ts_node_child_count(type_list);
        for (uint32_t i = 0; i < tnc; i++) {
            TSNode tc = ts_node_child(type_list, i);
            if (ts_node_is_null(tc) || !ts_node_is_named(tc)) continue;
            exc_type = php_parse_type_node(ctx, tc);
            break;
        }
    }
    cbm_scope_bind(ctx->current_scope, vname, exc_type);
}

/* ── call resolution ────────────────────────────────────────────── */

static void resolve_function_call(PHPLSPContext *ctx, TSNode call) {
    TSNode fn = ts_node_child_by_field_name(call, "function", 8);
    if (ts_node_is_null(fn)) return;
    char *name = php_node_text(ctx, fn);
    if (!name) return;

    /* `use function Foo\\bar;` mapping. */
    const char *target = find_use(ctx, name, CBM_PHP_USE_FUNCTION);
    if (target) {
        const CBMRegisteredFunc *f = cbm_registry_lookup_func(ctx->registry, target);
        if (f) {
            emit_resolved(ctx, f->qualified_name, "php_function_namespaced", 0.95f);
            return;
        }
    }

    /* Current namespace, then global fallback. */
    if (ctx->current_namespace_qn && ctx->current_namespace_qn[0]) {
        const char *ns_qn =
            cbm_arena_sprintf(ctx->arena, "%s.%s", ctx->current_namespace_qn, name);
        const CBMRegisteredFunc *f = cbm_registry_lookup_func(ctx->registry, ns_qn);
        if (f) {
            emit_resolved(ctx, f->qualified_name, "php_function_namespaced", 0.95f);
            return;
        }
    }

    /* Look for any free function with this short_name. Prefer the one in the
     * file's project (i.e. with the longest module-prefix overlap). */
    const CBMRegisteredFunc *best = NULL;
    int best_score = -1;
    for (int i = 0; ctx->registry && i < ctx->registry->func_count; i++) {
        const CBMRegisteredFunc *cand = &ctx->registry->funcs[i];
        if (cand->receiver_type) continue;
        if (!cand->short_name || strcmp(cand->short_name, name) != 0) continue;
        int score = 0;
        if (cand->qualified_name && ctx->module_qn) {
            const char *m = ctx->module_qn;
            const char *q = cand->qualified_name;
            while (*m && *q && *m == *q) {
                if (*m == '.') score++;
                m++;
                q++;
            }
        }
        if (score > best_score) {
            best_score = score;
            best = cand;
        }
    }
    if (best) {
        emit_resolved(ctx, best->qualified_name, "php_function_global_fallback", 0.70f);
        return;
    }
    /* Unresolved — but DO NOT emit unresolved noise here; the unified
     * extractor already records the call edge. We only contribute when we
     * can correct or refine attribution. */
    (void)emit_unresolved;
}

static void resolve_member_call(PHPLSPContext *ctx, TSNode call) {
    TSNode obj = ts_node_child_by_field_name(call, "object", 6);
    TSNode name_node = ts_node_child_by_field_name(call, "name", 4);
    if (ts_node_is_null(name_node)) return;
    char *method_name = php_node_text(ctx, name_node);
    if (!method_name) return;
    if (ts_node_is_null(obj)) return;

    const CBMType *recv = php_eval_expr_type(ctx, obj);
    if (!recv || recv->kind != CBM_TYPE_NAMED) return; /* unknown receiver; defer */

    const char *class_qn = recv->data.named.qualified_name;
    const CBMRegisteredFunc *f = php_lookup_method(ctx, class_qn, method_name);
    if (f) {
        const char *strategy = (f->receiver_type && strcmp(f->receiver_type, class_qn) == 0)
                                   ? "php_method_typed"
                                   : "php_method_inherited";
        emit_resolved(ctx, f->qualified_name, strategy, 0.95f);
        return;
    }
    /* Receiver known but method missing — magic __call? */
    if (class_has_magic_call(ctx, class_qn, false)) {
        emit_resolved(ctx,
                      cbm_arena_sprintf(ctx->arena, "%s.%s", class_qn, method_name),
                      "php_method_dynamic", 0.20f);
        return;
    }
    /* Receiver known but class not in registry (e.g. vendor type not indexed,
     * or method genuinely missing). Emit a synthetic resolved call pointing
     * at "<class_qn>.<method>" so the pipeline bridge can BLOCK the unified
     * extractor's likely-incorrect short-name fallback. The bridge filters
     * unknown targets and yields no edge — better than a wrong edge. */
    emit_resolved(ctx,
                  cbm_arena_sprintf(ctx->arena, "%s.%s", class_qn, method_name),
                  "php_method_typed_unindexed", 0.55f);
}

static void resolve_static_call(PHPLSPContext *ctx, TSNode call) {
    TSNode scope = ts_node_child_by_field_name(call, "scope", 5);
    TSNode name_node = ts_node_child_by_field_name(call, "name", 4);
    if (ts_node_is_null(name_node)) return;
    char *method_name = php_node_text(ctx, name_node);
    if (!method_name) return;

    const char *class_qn = NULL;
    const char *strategy = "php_static_resolved";
    if (!ts_node_is_null(scope)) {
        char *cls = php_node_text(ctx, scope);
        if (cls) {
            if (strcmp(cls, "self") == 0 || strcmp(cls, "static") == 0) {
                class_qn = ctx->enclosing_class_qn;
                strategy = "php_self_static";
            } else if (strcmp(cls, "parent") == 0) {
                class_qn = ctx->enclosing_parent_qn;
                strategy = "php_self_static";
            } else {
                class_qn = php_resolve_class_name(ctx, cls);
            }
        }
    }
    if (!class_qn) return;

    const CBMRegisteredFunc *f = php_lookup_method(ctx, class_qn, method_name);
    if (f) {
        emit_resolved(ctx, f->qualified_name, strategy, 0.95f);
        return;
    }
    /* Facade detection: class has __callStatic in its chain. */
    if (class_has_magic_call(ctx, class_qn, true)) {
        emit_resolved(ctx,
                      cbm_arena_sprintf(ctx->arena, "%s.%s", class_qn, method_name),
                      "php_dynamic_unresolved", 0.10f);
    }
}

/* Walk a subtree, binding scope and resolving calls. */
static void php_resolve_calls_in_node(PHPLSPContext *ctx, TSNode node) {
    if (ts_node_is_null(node)) return;
    const char *kind = ts_node_type(node);

    /* Stop descending into nested function-likes — they are processed by
     * process_function_like with their own scope. */
    if (strcmp(kind, "function_definition") == 0 ||
        strcmp(kind, "function_static_declaration") == 0 ||
        strcmp(kind, "method_declaration") == 0 || strcmp(kind, "anonymous_function") == 0 ||
        strcmp(kind, "arrow_function") == 0) {
        process_function_like(ctx, node);
        return;
    }

    /* Statement-level scope-binding observers. */
    if (strcmp(kind, "assignment_expression") == 0) {
        process_assignment(ctx, node);
    } else if (strcmp(kind, "foreach_statement") == 0) {
        process_foreach(ctx, node);
    } else if (strcmp(kind, "catch_clause") == 0) {
        process_catch(ctx, node);
    } else if (strcmp(kind, "comment") == 0) {
        char *raw = php_node_text(ctx, node);
        if (raw && raw[0] == '/' && raw[1] == '*' && raw[2] == '*') {
            char *cleaned = phpdoc_clean(ctx->arena, raw);
            bind_phpdoc_var(ctx, cleaned);
        }
    }

    /* Call-resolution dispatch. */
    if (strcmp(kind, "function_call_expression") == 0) {
        resolve_function_call(ctx, node);
    } else if (strcmp(kind, "member_call_expression") == 0 ||
               strcmp(kind, "nullsafe_member_call_expression") == 0) {
        resolve_member_call(ctx, node);
    } else if (strcmp(kind, "scoped_call_expression") == 0) {
        resolve_static_call(ctx, node);
    }

    /* Recurse. */
    uint32_t nc = ts_node_child_count(node);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_null(c)) php_resolve_calls_in_node(ctx, c);
    }
}

/* ── function-like processing ───────────────────────────────────── */

static void bind_typed_parameters(PHPLSPContext *ctx, TSNode params) {
    if (ts_node_is_null(params)) return;
    uint32_t nc = ts_node_child_count(params);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode p = ts_node_child(params, i);
        if (ts_node_is_null(p) || !ts_node_is_named(p)) continue;
        const char *pk = ts_node_type(p);
        if (strcmp(pk, "simple_parameter") != 0 &&
            strcmp(pk, "variadic_parameter") != 0 &&
            strcmp(pk, "property_promotion_parameter") != 0) {
            continue;
        }
        TSNode tnode = ts_node_child_by_field_name(p, "type", 4);
        const CBMType *ptype = cbm_type_unknown();
        if (!ts_node_is_null(tnode)) ptype = php_parse_type_node(ctx, tnode);
        TSNode name_node = ts_node_child_by_field_name(p, "name", 4);
        if (ts_node_is_null(name_node)) {
            /* fallback: first variable_name child */
            uint32_t pnc = ts_node_child_count(p);
            for (uint32_t j = 0; j < pnc; j++) {
                TSNode c = ts_node_child(p, j);
                if (!ts_node_is_null(c) && strcmp(ts_node_type(c), "variable_name") == 0) {
                    name_node = c;
                    break;
                }
            }
        }
        if (ts_node_is_null(name_node)) continue;
        char *vt = php_node_text(ctx, name_node);
        if (!vt) continue;
        const char *name = (vt[0] == '$') ? vt + 1 : vt;
        cbm_scope_bind(ctx->current_scope, name, ptype);
    }
}

static void process_function_like(PHPLSPContext *ctx, TSNode node) {
    const char *kind = ts_node_type(node);
    bool is_method = (strcmp(kind, "method_declaration") == 0);
    bool is_named = is_method || (strcmp(kind, "function_definition") == 0) ||
                    (strcmp(kind, "function_static_declaration") == 0);

    /* Save context. */
    CBMScope *saved_scope = ctx->current_scope;
    const char *saved_func = ctx->enclosing_func_qn;

    ctx->current_scope = cbm_scope_push(ctx->arena, ctx->current_scope);

    /* Determine func QN. Mirror what the unified extractor produces: classes
     * and free functions are namespaced by file module_qn, NOT the PHP
     * namespace declaration (the unified extractor ignores `namespace`).
     * Method QN = enclosing_class_qn + "." + method_name. Free function QN =
     * module_qn + "." + name. */
    if (is_named) {
        TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        if (!ts_node_is_null(name_node)) {
            char *fname = php_node_text(ctx, name_node);
            if (fname) {
                if (is_method && ctx->enclosing_class_qn) {
                    ctx->enclosing_func_qn =
                        cbm_arena_sprintf(ctx->arena, "%s.%s", ctx->enclosing_class_qn, fname);
                } else if (ctx->module_qn) {
                    ctx->enclosing_func_qn =
                        cbm_arena_sprintf(ctx->arena, "%s.%s", ctx->module_qn, fname);
                } else {
                    ctx->enclosing_func_qn = cbm_arena_strdup(ctx->arena, fname);
                }
            }
        }
    }

    /* Parameters. */
    TSNode params = ts_node_child_by_field_name(node, "parameters", 10);
    bind_typed_parameters(ctx, params);

    /* PHPDoc @param overrides. */
    if (is_named) {
        char *doc = fetch_leading_phpdoc(ctx, node);
        if (doc) parse_phpdoc_for_params(ctx, doc, params);
    }

    /* Walk body. */
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (ts_node_is_null(body)) {
        /* Arrow function body is in field "body" too, but it's an expression.
         * Try generic recursion as a fallback. */
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode c = ts_node_child(node, i);
            if (!ts_node_is_null(c)) php_resolve_calls_in_node(ctx, c);
        }
    } else {
        php_resolve_calls_in_node(ctx, body);
    }

    /* Restore. */
    ctx->current_scope = saved_scope;
    ctx->enclosing_func_qn = saved_func;
}

/* Find first base class (extends X) in a class_declaration, resolved. */
static const char *find_extends_qn(PHPLSPContext *ctx, TSNode class_node) {
    uint32_t nc = ts_node_child_count(class_node);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(class_node, i);
        if (ts_node_is_null(c) || !ts_node_is_named(c)) continue;
        if (strcmp(ts_node_type(c), "base_clause") != 0) continue;
        uint32_t bnc = ts_node_child_count(c);
        for (uint32_t j = 0; j < bnc; j++) {
            TSNode bc = ts_node_child(c, j);
            if (ts_node_is_null(bc) || !ts_node_is_named(bc)) continue;
            const char *bk = ts_node_type(bc);
            if (strcmp(bk, "name") == 0 || strcmp(bk, "qualified_name") == 0) {
                char *t = php_node_text(ctx, bc);
                if (t) return php_resolve_class_name(ctx, t);
            }
        }
    }
    return NULL;
}

static void process_class_decl(PHPLSPContext *ctx, TSNode node) {
    TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(name_node)) return;
    char *cname = php_node_text(ctx, name_node);
    if (!cname) return;

    const char *saved_class = ctx->enclosing_class_qn;
    const char *saved_parent = ctx->enclosing_parent_qn;

    /* enclosing_class_qn must match what the unified extractor records,
     * which uses module_qn (file-path-based), NOT the PHP `namespace`
     * declaration. */
    if (ctx->module_qn) {
        ctx->enclosing_class_qn =
            cbm_arena_sprintf(ctx->arena, "%s.%s", ctx->module_qn, cname);
    } else {
        ctx->enclosing_class_qn = cbm_arena_strdup(ctx->arena, cname);
    }
    ctx->enclosing_parent_qn = find_extends_qn(ctx, node);

    /* Walk class body — pick out method_declaration nodes. */
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (ts_node_is_null(body)) body = child_named(node, "declaration_list");
    if (!ts_node_is_null(body)) {
        uint32_t nc = ts_node_child_count(body);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode c = ts_node_child(body, i);
            if (ts_node_is_null(c)) continue;
            const char *k = ts_node_type(c);
            if (strcmp(k, "method_declaration") == 0) {
                process_function_like(ctx, c);
            }
        }
    }

    ctx->enclosing_class_qn = saved_class;
    ctx->enclosing_parent_qn = saved_parent;
}

/* ── top-level pass ─────────────────────────────────────────────── */

/* Collect a single namespace_use_clause's local→target mapping and add. */
static void collect_use_clause(PHPLSPContext *ctx, TSNode use_clause, int kind) {
    TSNode name_node;
    memset(&name_node, 0, sizeof(name_node));
    TSNode alias_node;
    memset(&alias_node, 0, sizeof(alias_node));
    uint32_t nc = ts_node_child_count(use_clause);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(use_clause, i);
        if (ts_node_is_null(c) || !ts_node_is_named(c)) continue;
        const char *k = ts_node_type(c);
        if (strcmp(k, "qualified_name") == 0 || strcmp(k, "name") == 0) {
            if (ts_node_is_null(name_node)) name_node = c;
        }
        if (strcmp(k, "namespace_aliasing_clause") == 0) {
            uint32_t ac = ts_node_child_count(c);
            for (uint32_t j = 0; j < ac; j++) {
                TSNode ach = ts_node_child(c, j);
                if (!ts_node_is_null(ach) && ts_node_is_named(ach) &&
                    strcmp(ts_node_type(ach), "name") == 0) {
                    alias_node = ach;
                    break;
                }
            }
        }
    }
    if (ts_node_is_null(name_node)) return;
    char *full = php_node_text(ctx, name_node);
    if (!full) return;
    char *dotted = php_ns_to_dot(ctx->arena, full);
    const char *local = NULL;
    if (!ts_node_is_null(alias_node)) {
        local = php_node_text(ctx, alias_node);
    }
    if (!local) {
        /* Default: last segment. */
        local = php_short_name(dotted);
    }
    php_lsp_add_use(ctx, local, dotted, kind);
}

static void collect_use_declaration(PHPLSPContext *ctx, TSNode use_decl) {
    /* Determine kind: function / const / class (default). */
    int kind = CBM_PHP_USE_CLASS;
    uint32_t nc = ts_node_child_count(use_decl);
    /* tree-sitter-php emits `function` or `const` keyword children. */
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(use_decl, i);
        if (ts_node_is_null(c)) continue;
        const char *k = ts_node_type(c);
        if (strcmp(k, "function") == 0) kind = CBM_PHP_USE_FUNCTION;
        if (strcmp(k, "const") == 0) kind = CBM_PHP_USE_CONST;
    }
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(use_decl, i);
        if (ts_node_is_null(c) || !ts_node_is_named(c)) continue;
        const char *k = ts_node_type(c);
        if (strcmp(k, "namespace_use_clause") == 0) {
            collect_use_clause(ctx, c, kind);
        } else if (strcmp(k, "namespace_use_group") == 0) {
            /* use App\\{Foo, Bar as B}; — recurse into clauses inside. */
            uint32_t gc = ts_node_child_count(c);
            for (uint32_t j = 0; j < gc; j++) {
                TSNode gch = ts_node_child(c, j);
                if (!ts_node_is_null(gch) && ts_node_is_named(gch) &&
                    strcmp(ts_node_type(gch), "namespace_use_clause") == 0) {
                    collect_use_clause(ctx, gch, kind);
                }
            }
        }
    }
}

static void set_namespace_from_decl(PHPLSPContext *ctx, TSNode ns_decl) {
    uint32_t nc = ts_node_child_count(ns_decl);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(ns_decl, i);
        if (ts_node_is_null(c) || !ts_node_is_named(c)) continue;
        const char *k = ts_node_type(c);
        if (strcmp(k, "namespace_name") == 0) {
            char *t = php_node_text(ctx, c);
            if (t) {
                ctx->current_namespace_qn = php_ns_to_dot(ctx->arena, t);
                return;
            }
        }
    }
}

void php_lsp_process_file(PHPLSPContext *ctx, TSNode root) {
    if (ts_node_is_null(root)) return;

    /* Pass 1: namespace + use declarations. */
    uint32_t nc = ts_node_child_count(root);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(root, i);
        if (ts_node_is_null(c)) continue;
        const char *k = ts_node_type(c);
        if (strcmp(k, "namespace_definition") == 0) {
            set_namespace_from_decl(ctx, c);
        } else if (strcmp(k, "namespace_use_declaration") == 0) {
            collect_use_declaration(ctx, c);
        }
    }

    /* Pass 2: process classes and top-level functions. */
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(root, i);
        if (ts_node_is_null(c)) continue;
        const char *k = ts_node_type(c);
        if (strcmp(k, "class_declaration") == 0 || strcmp(k, "trait_declaration") == 0 ||
            strcmp(k, "interface_declaration") == 0 || strcmp(k, "enum_declaration") == 0) {
            process_class_decl(ctx, c);
        } else if (strcmp(k, "function_definition") == 0 ||
                   strcmp(k, "function_static_declaration") == 0) {
            process_function_like(ctx, c);
        } else if (strcmp(k, "namespace_definition") == 0) {
            /* Body-style: namespace App { ... }. Walk children. */
            TSNode body = child_named(c, "declaration_list");
            if (!ts_node_is_null(body)) {
                uint32_t bn = ts_node_child_count(body);
                for (uint32_t j = 0; j < bn; j++) {
                    TSNode bc = ts_node_child(body, j);
                    if (ts_node_is_null(bc)) continue;
                    const char *bk = ts_node_type(bc);
                    if (strcmp(bk, "class_declaration") == 0 ||
                        strcmp(bk, "trait_declaration") == 0 ||
                        strcmp(bk, "interface_declaration") == 0 ||
                        strcmp(bk, "enum_declaration") == 0) {
                        process_class_decl(ctx, bc);
                    } else if (strcmp(bk, "function_definition") == 0) {
                        process_function_like(ctx, bc);
                    } else if (strcmp(bk, "expression_statement") == 0 ||
                               strcmp(bk, "echo_statement") == 0 ||
                               strcmp(bk, "if_statement") == 0) {
                        /* Top-level code; just walk. */
                        php_resolve_calls_in_node(ctx, bc);
                    }
                }
            }
        } else if (strcmp(k, "expression_statement") == 0 ||
                   strcmp(k, "if_statement") == 0 || strcmp(k, "echo_statement") == 0) {
            /* Top-level script code outside any function/class. Without an
             * enclosing function we cannot emit resolved calls, but we still
             * walk statements to populate scope for any subsequent function
             * processing in this file. */
            php_resolve_calls_in_node(ctx, c);
        }
    }
}

/* ── parse return-type text from CBMDefinition.return_type ──────── */

static const CBMType *parse_return_type_text(CBMArena *arena, const char *text,
                                              const char *module_qn,
                                              const char *current_namespace_qn,
                                              const char **use_local_names,
                                              const char **use_target_qns,
                                              int use_count) {
    if (!text || !*text) return cbm_type_unknown();
    /* Skip leading ":?" / "?". */
    while (*text == ':' || *text == ' ' || *text == '?') text++;
    if (php_is_builtin_type_name(text)) return cbm_type_builtin(arena, text);

    /* If union/intersection, take leftmost non-null. */
    const char *bar = strchr(text, '|');
    const char *amp = strchr(text, '&');
    const char *cut = NULL;
    if (bar && (!amp || bar < amp)) cut = bar;
    else if (amp) cut = amp;
    char *first;
    if (cut) {
        first = cbm_arena_strndup(arena, text, (size_t)(cut - text));
    } else {
        first = cbm_arena_strdup(arena, text);
    }
    if (!first) return cbm_type_unknown();
    /* trim. */
    while (*first == ' ' || *first == '?') first++;
    char *end = first + strlen(first);
    while (end > first && (end[-1] == ' ')) end--;
    *end = '\0';
    if (!*first || strcmp(first, "null") == 0) return cbm_type_unknown();
    if (php_is_builtin_type_name(first)) return cbm_type_builtin(arena, first);

    /* Resolve via use map. */
    if (first[0] == '\\') {
        return cbm_type_named(arena, php_ns_to_dot(arena, first + 1));
    }
    /* Find first segment for use lookup. */
    const char *bs = first;
    while (*bs && *bs != '\\') bs++;
    char *first_seg = (*bs) ? cbm_arena_strndup(arena, first, (size_t)(bs - first))
                             : cbm_arena_strdup(arena, first);
    for (int i = 0; i < use_count; i++) {
        if (strcmp(use_local_names[i], first_seg) == 0) {
            const char *target = use_target_qns[i];
            if (*bs) {
                return cbm_type_named(arena,
                                      cbm_arena_sprintf(arena, "%s%s", target,
                                                        php_ns_to_dot(arena, bs)));
            }
            return cbm_type_named(arena, target);
        }
    }
    /* Fallback: current namespace prefix. */
    if (current_namespace_qn && *current_namespace_qn) {
        return cbm_type_named(
            arena,
            cbm_arena_sprintf(arena, "%s.%s", current_namespace_qn, php_ns_to_dot(arena, first)));
    }
    if (module_qn) {
        return cbm_type_named(
            arena, cbm_arena_sprintf(arena, "%s.%s", module_qn, php_ns_to_dot(arena, first)));
    }
    return cbm_type_named(arena, php_ns_to_dot(arena, first));
}

/* ── entry: cbm_run_php_lsp ─────────────────────────────────────── */

void cbm_run_php_lsp(CBMArena *arena, CBMFileResult *result, const char *source, int source_len,
                     TSNode root) {
    if (!result || !arena || ts_node_is_null(root)) return;

    CBMTypeRegistry reg;
    cbm_registry_init(&reg, arena);

    /* Phase A: register stdlib types/functions. */
    cbm_php_stdlib_register(&reg, arena);

    const char *module_qn = result->module_qn;

    /* Phase B: register types and methods/functions from this file's defs.
     * We do not yet know the namespace mapping for the type's QN, so we
     * trust the QN that the unified extractor already produced. */
    for (int i = 0; i < result->defs.count; i++) {
        CBMDefinition *d = &result->defs.items[i];
        if (!d->qualified_name || !d->name || !d->label) continue;

        if (strcmp(d->label, "Class") == 0 || strcmp(d->label, "Interface") == 0 ||
            strcmp(d->label, "Trait") == 0 || strcmp(d->label, "Enum") == 0 ||
            strcmp(d->label, "Type") == 0) {
            CBMRegisteredType rt;
            memset(&rt, 0, sizeof(rt));
            rt.qualified_name = d->qualified_name;
            rt.short_name = d->name;
            rt.is_interface = (strcmp(d->label, "Interface") == 0);

            /* Hoist base_classes as embedded_types so php_lookup_method's
             * parent walk works. */
            if (d->base_classes) {
                int bc = 0;
                while (d->base_classes[bc]) bc++;
                if (bc > 0) {
                    const char **emb = (const char **)cbm_arena_alloc(
                        arena, (size_t)(bc + 1) * sizeof(const char *));
                    if (emb) {
                        for (int j = 0; j < bc; j++) {
                            const char *base = d->base_classes[j];
                            /* Try same-module, then bare. */
                            const char *qualified = base;
                            if (base[0] != '\\' && !strchr(base, '.')) {
                                qualified =
                                    cbm_arena_sprintf(arena, "%s.%s", module_qn, base);
                            } else if (base[0] == '\\') {
                                qualified = php_ns_to_dot(arena, base + 1);
                            } else {
                                qualified = php_ns_to_dot(arena, base);
                            }
                            emb[j] = qualified;
                        }
                        emb[bc] = NULL;
                        rt.embedded_types = emb;
                    }
                }
            }
            cbm_registry_add_type(&reg, rt);
        }

        if (strcmp(d->label, "Function") == 0 || strcmp(d->label, "Method") == 0) {
            CBMRegisteredFunc rf;
            memset(&rf, 0, sizeof(rf));
            rf.qualified_name = d->qualified_name;
            rf.short_name = d->name;
            if (strcmp(d->label, "Method") == 0 && d->parent_class) {
                rf.receiver_type = d->parent_class;
            }
            /* Build a minimal signature with just the return type. */
            const CBMType *ret_t =
                d->return_type
                    ? parse_return_type_text(arena, d->return_type, module_qn, NULL, NULL,
                                             NULL, 0)
                    : cbm_type_unknown();
            const CBMType **rets =
                (const CBMType **)cbm_arena_alloc(arena, 2 * sizeof(const CBMType *));
            if (rets) {
                rets[0] = ret_t;
                rets[1] = NULL;
            }
            rf.signature = cbm_type_func(arena, NULL, NULL, rets);
            cbm_registry_add_func(&reg, rf);
        }
    }

    /* Phase C: run the resolver. */
    PHPLSPContext ctx;
    php_lsp_init(&ctx, arena, source, source_len, &reg, module_qn, &result->resolved_calls);
    php_lsp_process_file(&ctx, root);

    if (ctx.debug) {
        fprintf(stderr, "[php_lsp] module=%s defs=%d types=%d funcs=%d resolved=%d\n",
                module_qn ? module_qn : "?", result->defs.count, reg.type_count,
                reg.func_count, result->resolved_calls.count);
        for (int i = 0; i < result->resolved_calls.count; i++) {
            const CBMResolvedCall *rc = &result->resolved_calls.items[i];
            fprintf(stderr, "[php_lsp]   %s -> %s [%s %.2f]\n",
                    rc->caller_qn ? rc->caller_qn : "?",
                    rc->callee_qn ? rc->callee_qn : "?",
                    rc->strategy ? rc->strategy : "?", rc->confidence);
        }
    }
}
