#ifndef CBM_LSP_TYPE_REP_H
#define CBM_LSP_TYPE_REP_H

#include "../arena.h"
#include <stdbool.h>
#include <stdint.h>

// CBMTypeKind enumerates all type representations.
typedef enum {
    CBM_TYPE_UNKNOWN = 0,
    CBM_TYPE_NAMED,       // named type: "Database", "http.Request"
    CBM_TYPE_POINTER,     // *T
    CBM_TYPE_SLICE,       // []T
    CBM_TYPE_MAP,         // map[K]V
    CBM_TYPE_CHANNEL,     // chan T
    CBM_TYPE_FUNC,        // func(params) returns
    CBM_TYPE_INTERFACE,   // interface{...}
    CBM_TYPE_STRUCT,      // struct{...}
    CBM_TYPE_BUILTIN,     // int, string, bool, error, etc.
    CBM_TYPE_TUPLE,       // multi-return (T1, T2)
    CBM_TYPE_TYPE_PARAM,  // generic type parameter: T, K, V
    CBM_TYPE_REFERENCE,   // T& (C++ lvalue reference)
    CBM_TYPE_RVALUE_REF,  // T&& (C++ rvalue reference)
    CBM_TYPE_TEMPLATE,    // Parameterized type: vector<T> — stores template name + args
    CBM_TYPE_ALIAS,       // Type alias: using/typedef — stores alias name + underlying type
    CBM_TYPE_UNION,       // Python: A | B, Optional[T], Union[A, B] — sorted-canonical list
    CBM_TYPE_LITERAL,     // Python: Literal["foo", 3] — wraps a base type + literal value text
    CBM_TYPE_PROTOCOL,    // Python: typing.Protocol — like INTERFACE but matched structurally
    CBM_TYPE_MODULE,      // Python: import os; os is a module-typed binding
    CBM_TYPE_CALLABLE,    // Python: Callable[[A, B], R] — untyped-named callable variant of FUNC
} CBMTypeKind;

// Forward declaration
typedef struct CBMType CBMType;

// CBMTypeParam represents a generic type parameter with optional constraint.
typedef struct {
    const char* name;        // "T", "K", "V"
    const CBMType* constraint; // interface constraint, or NULL for "any"
} CBMTypeParam;

// CBMType is a tagged union representing Go types.
struct CBMType {
    CBMTypeKind kind;
    union {
        struct { const char* qualified_name; } named;      // NAMED
        struct { const CBMType* elem; } pointer;            // POINTER
        struct { const CBMType* elem; } slice;              // SLICE
        struct { const CBMType* key; const CBMType* value; } map;  // MAP
        struct { const CBMType* elem; int direction; } channel;    // CHANNEL (0=bidi, 1=send, 2=recv)
        struct {
            const char** param_names;  // NULL-terminated
            const CBMType** param_types; // NULL-terminated
            const CBMType** return_types; // NULL-terminated
        } func;                                             // FUNC
        struct {
            const char** method_names;  // NULL-terminated
            const CBMType** method_sigs; // NULL-terminated (each is FUNC)
        } interface_type;                                   // INTERFACE
        struct {
            const char** field_names;   // NULL-terminated
            const CBMType** field_types; // NULL-terminated
        } struct_type;                                      // STRUCT
        struct { const char* name; } builtin;               // BUILTIN
        struct {
            const CBMType** elems;      // NULL-terminated
            int count;
        } tuple;                                            // TUPLE
        struct { const char* name; } type_param;            // TYPE_PARAM
        struct { const CBMType* elem; } reference;            // REFERENCE / RVALUE_REF
        struct {
            const char* template_name;      // "std::vector", "std::map"
            const CBMType** template_args;  // NULL-terminated
            int arg_count;
        } template_type;                                      // TEMPLATE
        struct {
            const char* alias_qn;          // "proj.ns.MyAlias"
            const CBMType* underlying;     // the actual type it aliases
        } alias;                                              // ALIAS
        struct {
            const CBMType** members;       // NULL-terminated, deduplicated, sorted by kind/qn
            int count;
        } union_type;                                         // UNION
        struct {
            const CBMType* base;           // base type (e.g. BUILTIN("int"), BUILTIN("str"))
            const char* literal_text;      // canonical text: "3", "\"foo\"", "True"
        } literal;                                            // LITERAL
        struct {
            const char* qualified_name;    // e.g. "typing.Iterable"
            const char** method_names;     // NULL-terminated method names — structural matching
            const CBMType** method_sigs;   // NULL-terminated signatures (each is FUNC/CALLABLE)
        } protocol;                                           // PROTOCOL
        struct {
            const char* module_qn;         // module qualified name (matches CBMImport.module_path)
        } module;                                             // MODULE
        struct {
            const CBMType** param_types;   // NULL-terminated; NULL element means "Any" / unknown
            const CBMType* return_type;    // single return; for tuples wrap in CBM_TYPE_TUPLE
            int param_count;               // -1 = elliptic / Callable[..., R]
        } callable;                                           // CALLABLE
    } data;
};

// Constructors (arena-allocated)
const CBMType* cbm_type_unknown(void);
const CBMType* cbm_type_named(CBMArena* a, const char* qualified_name);
const CBMType* cbm_type_pointer(CBMArena* a, const CBMType* elem);
const CBMType* cbm_type_slice(CBMArena* a, const CBMType* elem);
const CBMType* cbm_type_map(CBMArena* a, const CBMType* key, const CBMType* value);
const CBMType* cbm_type_channel(CBMArena* a, const CBMType* elem, int direction);
const CBMType* cbm_type_func(CBMArena* a, const char** param_names, const CBMType** param_types, const CBMType** return_types);
const CBMType* cbm_type_builtin(CBMArena* a, const char* name);
const CBMType* cbm_type_tuple(CBMArena* a, const CBMType** elems, int count);
const CBMType* cbm_type_type_param(CBMArena* a, const char* name);
const CBMType* cbm_type_reference(CBMArena* a, const CBMType* elem);
const CBMType* cbm_type_rvalue_ref(CBMArena* a, const CBMType* elem);
const CBMType* cbm_type_template(CBMArena* a, const char* name, const CBMType** args, int arg_count);
const CBMType* cbm_type_alias(CBMArena* a, const char* alias_qn, const CBMType* underlying);

// Python-flavored constructors. UNION normalizes input: nested unions are
// flattened, duplicates removed, single-member unions collapse to that
// member, and the empty union is UNKNOWN. Members must be arena-allocated.
const CBMType* cbm_type_union(CBMArena* a, const CBMType** members, int count);
const CBMType* cbm_type_optional(CBMArena* a, const CBMType* t);  // Optional[T] == Union[T, None]
const CBMType* cbm_type_literal(CBMArena* a, const CBMType* base, const char* literal_text);
const CBMType* cbm_type_protocol(CBMArena* a, const char* qualified_name,
    const char** method_names, const CBMType** method_sigs);
const CBMType* cbm_type_module(CBMArena* a, const char* module_qn);
const CBMType* cbm_type_callable(CBMArena* a, const CBMType** param_types, int param_count,
    const CBMType* return_type);

// Operations
const CBMType* cbm_type_deref(const CBMType* t);         // remove one pointer level
const CBMType* cbm_type_elem(const CBMType* t);           // get element type (slice/chan/pointer)
bool cbm_type_is_unknown(const CBMType* t);
bool cbm_type_is_interface(const CBMType* t);
bool cbm_type_is_pointer(const CBMType* t);
bool cbm_type_is_reference(const CBMType* t);
bool cbm_type_is_union(const CBMType* t);
bool cbm_type_is_protocol(const CBMType* t);
bool cbm_type_is_module(const CBMType* t);

// Structural equality on type representation (used by union dedup and
// protocol-method-set matching). Two types are equal if their kinds match
// and their structural members match recursively.
bool cbm_type_equal(const CBMType* a, const CBMType* b);

// Test whether `candidate` satisfies the structural protocol `proto`.
// Walks proto.method_names against candidate's method set (NAMED → registry
// lookup is the caller's job; this helper only matches existing method
// signatures stored on a PROTOCOL).
bool cbm_type_protocol_satisfied_by(const CBMType* proto, const CBMType* candidate);

// Follow alias chain with cycle detection (max 16 levels).
const CBMType* cbm_type_resolve_alias(const CBMType* t);

// Generic type substitution: replace type params in t with concrete types.
// type_params: NULL-terminated array of param names
// type_args: corresponding concrete types
const CBMType* cbm_type_substitute(CBMArena* a, const CBMType* t,
    const char** type_params, const CBMType** type_args);

#endif // CBM_LSP_TYPE_REP_H
