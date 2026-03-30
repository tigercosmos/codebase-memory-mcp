#include "cbm.h"
#include "helpers.h"
#include "lang_specs.h"
#include "extract_unified.h"
#include "tree_sitter/api.h" // TSNode, ts_node_*
#include <stdint.h>          // uint32_t
#include <string.h>
#include <ctype.h>

// Field name length for ts_node_child_by_field_name() calls.
#define FIELD_LEN_CONSTRUCTOR 11 // strlen("constructor")

// --- Throw/Raise extraction ---

// Process a single node for throw extraction (called from iterative walker).
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void process_throw_node(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec) {
    const char *kind = ts_node_type(node);

    if (cbm_kind_in_set(node, spec->throw_node_types)) {
        char *exc_name = NULL;
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode child = ts_node_child(node, i);
            const char *ck = ts_node_type(child);
            if (strcmp(ck, "raise") == 0 || strcmp(ck, "throw") == 0) {
                continue;
            }
            if (ck[0] == ';' || ck[0] == '(' || ck[0] == ')') {
                continue;
            }
            if (strcmp(ck, "call") == 0 || strcmp(ck, "call_expression") == 0 ||
                strcmp(ck, "new_expression") == 0 ||
                strcmp(ck, "object_creation_expression") == 0 ||
                strcmp(ck, "instance_expression") == 0) {
                TSNode fn = ts_node_child_by_field_name(child, "function", 8);
                if (ts_node_is_null(fn)) {
                    fn = ts_node_child_by_field_name(child, "constructor", FIELD_LEN_CONSTRUCTOR);
                }
                if (ts_node_is_null(fn)) {
                    fn = ts_node_child_by_field_name(child, "type", 4);
                }
                if (ts_node_is_null(fn) && ts_node_named_child_count(child) > 0) {
                    fn = ts_node_named_child(child, 0);
                }
                if (!ts_node_is_null(fn)) {
                    exc_name = cbm_node_text(ctx->arena, fn, ctx->source);
                }
            } else {
                exc_name = cbm_node_text(ctx->arena, child, ctx->source);
            }
            break;
        }
        if (exc_name && exc_name[0]) {
            if (strlen(exc_name) > 100) {
                exc_name[100] = '\0';
            }
            CBMThrow thr;
            thr.exception_name = exc_name;
            thr.enclosing_func_qn = cbm_enclosing_func_qn_cached(ctx, node);
            cbm_throws_push(&ctx->result->throws, ctx->arena, thr);
        }
    }

    if (spec->throws_clause_field && spec->throws_clause_field[0]) {
        if (strcmp(kind, "method_declaration") == 0 ||
            strcmp(kind, "constructor_declaration") == 0) {
            TSNode throws_clause = ts_node_child_by_field_name(
                node, spec->throws_clause_field, (uint32_t)strlen(spec->throws_clause_field));
            if (!ts_node_is_null(throws_clause)) {
                uint32_t nc = ts_node_child_count(throws_clause);
                for (uint32_t i = 0; i < nc; i++) {
                    TSNode child = ts_node_child(throws_clause, i);
                    const char *ck = ts_node_type(child);
                    if (strcmp(ck, "type_identifier") == 0 || strcmp(ck, "identifier") == 0 ||
                        strcmp(ck, "scoped_type_identifier") == 0) {
                        char *exc = cbm_node_text(ctx->arena, child, ctx->source);
                        if (exc && exc[0]) {
                            const char *func_qn = cbm_enclosing_func_qn_cached(ctx, node);
                            CBMThrow thr = {.exception_name = exc, .enclosing_func_qn = func_qn};
                            cbm_throws_push(&ctx->result->throws, ctx->arena, thr);
                        }
                    }
                }
            }
        }
    }
}

// Iterative throw walker
#define THROWS_STACK_CAP 512
static void walk_throws(CBMExtractCtx *ctx, TSNode root, const CBMLangSpec *spec) {
    TSNode stack[THROWS_STACK_CAP];
    int top = 0;
    stack[top++] = root;
    while (top > 0) {
        TSNode node = stack[--top];
        process_throw_node(ctx, node, spec);
        uint32_t count = ts_node_child_count(node);
        for (int i = (int)count - 1; i >= 0 && top < THROWS_STACK_CAP; i--) {
            stack[top++] = ts_node_child(node, (uint32_t)i);
        }
    }
}

// --- Read/Write detection (iterative) ---

#define READWRITE_STACK_CAP 512
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void walk_readwrites(CBMExtractCtx *ctx, TSNode root, const CBMLangSpec *spec) {
    TSNode stack[READWRITE_STACK_CAP];
    int top = 0;
    stack[top++] = root;
    while (top > 0) {
        TSNode node = stack[--top];
        if (cbm_kind_in_set(node, spec->assignment_node_types)) {
            TSNode left = ts_node_child_by_field_name(node, "left", 4);
            if (ts_node_is_null(left)) {
                if (ts_node_child_count(node) > 0) {
                    left = ts_node_child(node, 0);
                }
            }
            if (!ts_node_is_null(left)) {
                const char *lk = ts_node_type(left);
                if (strcmp(lk, "identifier") == 0 || strcmp(lk, "simple_identifier") == 0) {
                    char *name = cbm_node_text(ctx->arena, left, ctx->source);
                    if (name && name[0] && !cbm_is_keyword(name, ctx->language)) {
                        CBMReadWrite rw;
                        rw.var_name = name;
                        rw.is_write = true;
                        rw.enclosing_func_qn = cbm_enclosing_func_qn_cached(ctx, node);
                        cbm_rw_push(&ctx->result->rw, ctx->arena, rw);
                    }
                }
            }
        }
        uint32_t count = ts_node_child_count(node);
        for (int i = (int)count - 1; i >= 0 && top < READWRITE_STACK_CAP; i--) {
            stack[top++] = ts_node_child(node, (uint32_t)i);
        }
    }
}

void cbm_extract_semantic(CBMExtractCtx *ctx) {
    const CBMLangSpec *spec = cbm_lang_spec(ctx->language);
    if (!spec) {
        return;
    }

    // Throws
    if ((spec->throw_node_types && spec->throw_node_types[0]) ||
        (spec->throws_clause_field && spec->throws_clause_field[0])) {
        walk_throws(ctx, ctx->root, spec);
    }

    // Reads/Writes
    if (spec->assignment_node_types && spec->assignment_node_types[0]) {
        walk_readwrites(ctx, ctx->root, spec);
    }
}

// --- Unified handlers ---

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void handle_throws(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state) {
    bool has_throws = spec->throw_node_types && spec->throw_node_types[0];
    bool has_clause = spec->throws_clause_field && spec->throws_clause_field[0];
    if (!has_throws && !has_clause) {
        return;
    }

    const char *kind = ts_node_type(node);

    if (has_throws && cbm_kind_in_set(node, spec->throw_node_types)) {
        char *exc_name = NULL;
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode child = ts_node_child(node, i);
            const char *ck = ts_node_type(child);
            if (strcmp(ck, "raise") == 0 || strcmp(ck, "throw") == 0) {
                continue;
            }
            if (ck[0] == ';' || ck[0] == '(' || ck[0] == ')') {
                continue;
            }

            if (strcmp(ck, "call") == 0 || strcmp(ck, "call_expression") == 0 ||
                strcmp(ck, "new_expression") == 0 ||
                strcmp(ck, "object_creation_expression") == 0 ||
                strcmp(ck, "instance_expression") == 0) {
                TSNode fn = ts_node_child_by_field_name(child, "function", 8);
                if (ts_node_is_null(fn)) {
                    fn = ts_node_child_by_field_name(child, "constructor", FIELD_LEN_CONSTRUCTOR);
                }
                if (ts_node_is_null(fn)) {
                    fn = ts_node_child_by_field_name(child, "type", 4);
                }
                if (ts_node_is_null(fn) && ts_node_named_child_count(child) > 0) {
                    fn = ts_node_named_child(child, 0);
                }
                if (!ts_node_is_null(fn)) {
                    exc_name = cbm_node_text(ctx->arena, fn, ctx->source);
                }
            } else {
                // identifier, type_identifier, or any other expression
                exc_name = cbm_node_text(ctx->arena, child, ctx->source);
            }
            break;
        }

        if (exc_name && exc_name[0]) {
            if (strlen(exc_name) > 100) {
                exc_name[100] = '\0';
            }
            CBMThrow thr;
            thr.exception_name = exc_name;
            thr.enclosing_func_qn = state->enclosing_func_qn;
            cbm_throws_push(&ctx->result->throws, ctx->arena, thr);
        }
    }

    // Java: throws clause on method/constructor declarations
    if (has_clause) {
        if (strcmp(kind, "method_declaration") == 0 ||
            strcmp(kind, "constructor_declaration") == 0) {
            TSNode throws_clause = ts_node_child_by_field_name(
                node, spec->throws_clause_field, (uint32_t)strlen(spec->throws_clause_field));
            if (!ts_node_is_null(throws_clause)) {
                uint32_t nc = ts_node_child_count(throws_clause);
                for (uint32_t i = 0; i < nc; i++) {
                    TSNode child = ts_node_child(throws_clause, i);
                    const char *ck = ts_node_type(child);
                    if (strcmp(ck, "type_identifier") == 0 || strcmp(ck, "identifier") == 0 ||
                        strcmp(ck, "scoped_type_identifier") == 0) {
                        char *exc = cbm_node_text(ctx->arena, child, ctx->source);
                        if (exc && exc[0]) {
                            CBMThrow thr = {.exception_name = exc,
                                            .enclosing_func_qn = state->enclosing_func_qn};
                            cbm_throws_push(&ctx->result->throws, ctx->arena, thr);
                        }
                    }
                }
            }
        }
    }
}

void handle_readwrites(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state) {
    if (!spec->assignment_node_types || !spec->assignment_node_types[0]) {
        return;
    }

    if (cbm_kind_in_set(node, spec->assignment_node_types)) {
        TSNode left = ts_node_child_by_field_name(node, "left", 4);
        if (ts_node_is_null(left)) {
            if (ts_node_child_count(node) > 0) {
                left = ts_node_child(node, 0);
            }
        }

        if (!ts_node_is_null(left)) {
            const char *lk = ts_node_type(left);
            if (strcmp(lk, "identifier") == 0 || strcmp(lk, "simple_identifier") == 0) {
                char *name = cbm_node_text(ctx->arena, left, ctx->source);
                if (name && name[0] && !cbm_is_keyword(name, ctx->language)) {
                    CBMReadWrite rw;
                    rw.var_name = name;
                    rw.is_write = true;
                    rw.enclosing_func_qn = state->enclosing_func_qn;
                    cbm_rw_push(&ctx->result->rw, ctx->arena, rw);
                }
            }
        }
    }
}
