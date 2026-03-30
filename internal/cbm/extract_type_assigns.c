#include "cbm.h"
#include "arena.h" // CBMArena
#include "helpers.h"
#include "lang_specs.h"
#include "extract_unified.h"
#include "tree_sitter/api.h" // TSNode, ts_node_*
#include <stdint.h>          // uint32_t
#include <string.h>
#include <ctype.h>

// Extract class/type name from a constructor expression.
// e.g., new Foo() -> "Foo", Foo() -> "Foo" (if uppercase), Foo{} -> "Foo"
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static const char *extract_constructor_type(CBMArena *a, TSNode rhs, const char *source,
                                            CBMLanguage lang) {
    const char *kind = ts_node_type(rhs);

    // new_expression / object_creation_expression -> type field or first child
    if (strcmp(kind, "new_expression") == 0 || strcmp(kind, "object_creation_expression") == 0) {
        TSNode type_node = ts_node_child_by_field_name(rhs, "type", 4);
        if (!ts_node_is_null(type_node)) {
            const char *tk = ts_node_type(type_node);
            if (strcmp(tk, "type_identifier") == 0 || strcmp(tk, "identifier") == 0 ||
                strcmp(tk, "simple_identifier") == 0) {
                return cbm_node_text(a, type_node, source);
            }
            // generic_type: get base
            if (strcmp(tk, "generic_type") == 0 && ts_node_child_count(type_node) > 0) {
                return cbm_node_text(a, ts_node_child(type_node, 0), source);
            }
            return cbm_node_text(a, type_node, source);
        }
        // Fallback: first named child
        for (uint32_t i = 0; i < ts_node_child_count(rhs); i++) {
            TSNode child = ts_node_child(rhs, i);
            const char *ck = ts_node_type(child);
            if (strcmp(ck, "identifier") == 0 || strcmp(ck, "type_identifier") == 0 ||
                strcmp(ck, "simple_identifier") == 0) {
                return cbm_node_text(a, child, source);
            }
        }
    }

    // call_expression where function is an uppercase identifier (Python/Kotlin/Scala class
    // construction)
    if (strcmp(kind, "call") == 0 || strcmp(kind, "call_expression") == 0) {
        TSNode func = ts_node_child_by_field_name(rhs, "function", 8);
        if (ts_node_is_null(func) && ts_node_child_count(rhs) > 0) {
            func = ts_node_child(rhs, 0);
        }
        if (!ts_node_is_null(func)) {
            char *fname = cbm_node_text(a, func, source);
            if (fname && fname[0] >= 'A' && fname[0] <= 'Z') {
                return fname;
            }
        }
    }

    // Go composite_literal: Type{...}
    if (strcmp(kind, "composite_literal") == 0) {
        TSNode type_node = ts_node_child_by_field_name(rhs, "type", 4);
        if (!ts_node_is_null(type_node)) {
            return cbm_node_text(a, type_node, source);
        }
    }

    // Rust: Type::new() or Type { ... }
    if (lang == CBM_LANG_RUST) {
        if (strcmp(kind, "struct_expression") == 0) {
            TSNode name = ts_node_child_by_field_name(rhs, "name", 4);
            if (!ts_node_is_null(name)) {
                return cbm_node_text(a, name, source);
            }
        }
    }

    return NULL;
}

// Walk AST for assignment patterns where RHS is a constructor call.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void walk_type_assigns(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec) {
    const char *kind = ts_node_type(node);

    // Assignment: var = Constructor()
    if (cbm_kind_in_set(node, spec->assignment_node_types)) {
        TSNode left = ts_node_child_by_field_name(node, "left", 4);
        TSNode right = ts_node_child_by_field_name(node, "right", 5);
        if (ts_node_is_null(right)) {
            right = ts_node_child_by_field_name(node, "value", 5);
        }
        if (!ts_node_is_null(left) && !ts_node_is_null(right)) {
            const char *lk = ts_node_type(left);
            if (strcmp(lk, "identifier") == 0 || strcmp(lk, "simple_identifier") == 0) {
                char *var_name = cbm_node_text(ctx->arena, left, ctx->source);
                const char *type_name =
                    extract_constructor_type(ctx->arena, right, ctx->source, ctx->language);
                if (var_name && var_name[0] && type_name && type_name[0]) {
                    CBMTypeAssign ta;
                    ta.var_name = var_name;
                    ta.type_name = type_name;
                    ta.enclosing_func_qn = cbm_enclosing_func_qn_cached(ctx, node);
                    cbm_typeassign_push(&ctx->result->type_assigns, ctx->arena, ta);
                }
            }
        }
    }

    // Variable declarations with constructor RHS
    // Go: short_var_declaration, var_spec
    // JS/TS: variable_declarator (const x = new Foo())
    // Python: assignment already handled above
    // Rust: let_declaration
    if (strcmp(kind, "short_var_declaration") == 0 || strcmp(kind, "var_spec") == 0) {
        TSNode left = ts_node_child_by_field_name(node, "name", 4);
        if (ts_node_is_null(left)) {
            left = ts_node_child_by_field_name(node, "left", 4);
        }
        TSNode right = ts_node_child_by_field_name(node, "value", 5);
        if (ts_node_is_null(right)) {
            right = ts_node_child_by_field_name(node, "right", 5);
        }
        if (!ts_node_is_null(left) && !ts_node_is_null(right)) {
            char *var_name = cbm_node_text(ctx->arena, left, ctx->source);
            const char *type_name =
                extract_constructor_type(ctx->arena, right, ctx->source, ctx->language);
            if (var_name && var_name[0] && type_name && type_name[0]) {
                CBMTypeAssign ta;
                ta.var_name = var_name;
                ta.type_name = type_name;
                ta.enclosing_func_qn = cbm_enclosing_func_qn_cached(ctx, node);
                cbm_typeassign_push(&ctx->result->type_assigns, ctx->arena, ta);
            }
        }
    }

    if (strcmp(kind, "variable_declarator") == 0) {
        TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        TSNode value_node = ts_node_child_by_field_name(node, "value", 5);
        if (!ts_node_is_null(name_node) && !ts_node_is_null(value_node)) {
            const char *nk = ts_node_type(name_node);
            if (strcmp(nk, "identifier") == 0 || strcmp(nk, "simple_identifier") == 0) {
                char *var_name = cbm_node_text(ctx->arena, name_node, ctx->source);
                const char *type_name =
                    extract_constructor_type(ctx->arena, value_node, ctx->source, ctx->language);
                if (var_name && var_name[0] && type_name && type_name[0]) {
                    CBMTypeAssign ta;
                    ta.var_name = var_name;
                    ta.type_name = type_name;
                    ta.enclosing_func_qn = cbm_enclosing_func_qn_cached(ctx, node);
                    cbm_typeassign_push(&ctx->result->type_assigns, ctx->arena, ta);
                }
            }
        }
    }

    if (strcmp(kind, "let_declaration") == 0 && ctx->language == CBM_LANG_RUST) {
        TSNode pat = ts_node_child_by_field_name(node, "pattern", 7);
        TSNode val = ts_node_child_by_field_name(node, "value", 5);
        if (!ts_node_is_null(pat) && !ts_node_is_null(val)) {
            if (strcmp(ts_node_type(pat), "identifier") == 0) {
                char *var_name = cbm_node_text(ctx->arena, pat, ctx->source);
                const char *type_name =
                    extract_constructor_type(ctx->arena, val, ctx->source, ctx->language);
                if (var_name && var_name[0] && type_name && type_name[0]) {
                    CBMTypeAssign ta;
                    ta.var_name = var_name;
                    ta.type_name = type_name;
                    ta.enclosing_func_qn = cbm_enclosing_func_qn_cached(ctx, node);
                    cbm_typeassign_push(&ctx->result->type_assigns, ctx->arena, ta);
                }
            }
        }
    }

    // Recurse
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; i++) {
        walk_type_assigns(ctx, ts_node_child(node, i), spec);
    }
}

void cbm_extract_type_assigns(CBMExtractCtx *ctx) {
    const CBMLangSpec *spec = cbm_lang_spec(ctx->language);
    if (!spec) {
        return;
    }

    walk_type_assigns(ctx, ctx->root, spec);
}

// --- Unified handler ---

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void handle_type_assigns(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec,
                         WalkState *state) {
    const char *kind = ts_node_type(node);

    // Assignment: var = Constructor()
    if (spec->assignment_node_types && cbm_kind_in_set(node, spec->assignment_node_types)) {
        TSNode left = ts_node_child_by_field_name(node, "left", 4);
        TSNode right = ts_node_child_by_field_name(node, "right", 5);
        if (ts_node_is_null(right)) {
            right = ts_node_child_by_field_name(node, "value", 5);
        }
        if (!ts_node_is_null(left) && !ts_node_is_null(right)) {
            const char *lk = ts_node_type(left);
            if (strcmp(lk, "identifier") == 0 || strcmp(lk, "simple_identifier") == 0) {
                char *var_name = cbm_node_text(ctx->arena, left, ctx->source);
                const char *type_name =
                    extract_constructor_type(ctx->arena, right, ctx->source, ctx->language);
                if (var_name && var_name[0] && type_name && type_name[0]) {
                    CBMTypeAssign ta;
                    ta.var_name = var_name;
                    ta.type_name = type_name;
                    ta.enclosing_func_qn = state->enclosing_func_qn;
                    cbm_typeassign_push(&ctx->result->type_assigns, ctx->arena, ta);
                }
            }
        }
    }

    // Go: short_var_declaration, var_spec
    if (strcmp(kind, "short_var_declaration") == 0 || strcmp(kind, "var_spec") == 0) {
        TSNode left = ts_node_child_by_field_name(node, "name", 4);
        if (ts_node_is_null(left)) {
            left = ts_node_child_by_field_name(node, "left", 4);
        }
        TSNode right = ts_node_child_by_field_name(node, "value", 5);
        if (ts_node_is_null(right)) {
            right = ts_node_child_by_field_name(node, "right", 5);
        }
        if (!ts_node_is_null(left) && !ts_node_is_null(right)) {
            char *var_name = cbm_node_text(ctx->arena, left, ctx->source);
            const char *type_name =
                extract_constructor_type(ctx->arena, right, ctx->source, ctx->language);
            if (var_name && var_name[0] && type_name && type_name[0]) {
                CBMTypeAssign ta;
                ta.var_name = var_name;
                ta.type_name = type_name;
                ta.enclosing_func_qn = state->enclosing_func_qn;
                cbm_typeassign_push(&ctx->result->type_assigns, ctx->arena, ta);
            }
        }
    }

    // JS/TS: variable_declarator
    if (strcmp(kind, "variable_declarator") == 0) {
        TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        TSNode value_node = ts_node_child_by_field_name(node, "value", 5);
        if (!ts_node_is_null(name_node) && !ts_node_is_null(value_node)) {
            const char *nk = ts_node_type(name_node);
            if (strcmp(nk, "identifier") == 0 || strcmp(nk, "simple_identifier") == 0) {
                char *var_name = cbm_node_text(ctx->arena, name_node, ctx->source);
                const char *type_name =
                    extract_constructor_type(ctx->arena, value_node, ctx->source, ctx->language);
                if (var_name && var_name[0] && type_name && type_name[0]) {
                    CBMTypeAssign ta;
                    ta.var_name = var_name;
                    ta.type_name = type_name;
                    ta.enclosing_func_qn = state->enclosing_func_qn;
                    cbm_typeassign_push(&ctx->result->type_assigns, ctx->arena, ta);
                }
            }
        }
    }

    // Rust: let_declaration
    if (strcmp(kind, "let_declaration") == 0 && ctx->language == CBM_LANG_RUST) {
        TSNode pat = ts_node_child_by_field_name(node, "pattern", 7);
        TSNode val = ts_node_child_by_field_name(node, "value", 5);
        if (!ts_node_is_null(pat) && !ts_node_is_null(val)) {
            if (strcmp(ts_node_type(pat), "identifier") == 0) {
                char *var_name = cbm_node_text(ctx->arena, pat, ctx->source);
                const char *type_name =
                    extract_constructor_type(ctx->arena, val, ctx->source, ctx->language);
                if (var_name && var_name[0] && type_name && type_name[0]) {
                    CBMTypeAssign ta;
                    ta.var_name = var_name;
                    ta.type_name = type_name;
                    ta.enclosing_func_qn = state->enclosing_func_qn;
                    cbm_typeassign_push(&ctx->result->type_assigns, ctx->arena, ta);
                }
            }
        }
    }
}
