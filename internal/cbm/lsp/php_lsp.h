#ifndef CBM_LSP_PHP_LSP_H
#define CBM_LSP_PHP_LSP_H

#include "type_rep.h"
#include "scope.h"
#include "type_registry.h"
#include "../cbm.h"

/* PHPLSPContext — per-file state for PHP type-aware call resolution.
 * Mirrors GoLSPContext / CLSPContext structure. */
typedef struct {
    CBMArena *arena;
    const char *source;
    int source_len;
    const CBMTypeRegistry *registry;
    CBMScope *current_scope;

    /* Namespace state. PHP files declare a single namespace
     * (or use the global namespace if none); empty string means global. */
    const char *current_namespace_qn;

    /* `use` clause map.
     * use_kinds[i] selects whether the local maps a class, function, or const. */
    const char **use_local_names;
    const char **use_target_qns;
    enum { CBM_PHP_USE_CLASS = 0, CBM_PHP_USE_FUNCTION, CBM_PHP_USE_CONST } *use_kinds;
    int use_count;
    int use_cap;

    /* Current function/method/class context. */
    const char *enclosing_func_qn;
    const char *enclosing_class_qn; /* NULL outside class body */
    const char *enclosing_parent_qn; /* parent class QN (for parent::), or NULL */
    const char *module_qn;

    /* Output: resolved calls accumulate here. */
    CBMResolvedCallArray *resolved_calls;

    /* Recursion guard for php_eval_expr_type. */
    int eval_depth;

    /* Debug mode (CBM_LSP_DEBUG env). */
    bool debug;
} PHPLSPContext;

/* Initialize a PHPLSPContext for processing one file. */
void php_lsp_init(PHPLSPContext *ctx, CBMArena *arena, const char *source, int source_len,
                  const CBMTypeRegistry *registry, const char *module_qn,
                  CBMResolvedCallArray *out);

/* Add a `use` mapping. */
void php_lsp_add_use(PHPLSPContext *ctx, const char *local_name, const char *target_qn,
                     int use_kind);

/* Process a file's AST: walk top-level decls, then function/method bodies. */
void php_lsp_process_file(PHPLSPContext *ctx, TSNode root);

/* Evaluate a PHP expression's type. May return NULL / CBM_TYPE_UNKNOWN. */
const CBMType *php_eval_expr_type(PHPLSPContext *ctx, TSNode node);

/* Parse a PHP type-AST node (named_type, primitive_type, union_type, ...) to CBMType. */
const CBMType *php_parse_type_node(PHPLSPContext *ctx, TSNode node);

/* Resolve a class name (bare or qualified) using current namespace + use map. */
const char *php_resolve_class_name(PHPLSPContext *ctx, const char *name);

/* Look up a method on a class, walking parent chain (registry-based). */
const CBMRegisteredFunc *php_lookup_method(PHPLSPContext *ctx, const char *class_qn,
                                            const char *method_name);

/* Entry point: build registry from file defs + stdlib + composer (if present),
 * then run resolution. Called from cbm_extract_file(). */
void cbm_run_php_lsp(CBMArena *arena, CBMFileResult *result, const char *source, int source_len,
                    TSNode root);

/* Register PHP stdlib + curated framework types into a registry. */
void cbm_php_stdlib_register(CBMTypeRegistry *reg, CBMArena *arena);

#endif /* CBM_LSP_PHP_LSP_H */
