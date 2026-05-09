#ifndef CBM_LSP_TYPE_REGISTRY_H
#define CBM_LSP_TYPE_REGISTRY_H

#include "type_rep.h"
#include "../arena.h"

// Decorator-derived flags (Python). Added at struct tail so existing
// callers that memset to zero before populating other fields keep working.
typedef enum {
    CBM_FUNC_FLAG_NONE         = 0,
    CBM_FUNC_FLAG_PROPERTY     = 1 << 0,  // @property -> obj.attr returns getter return
    CBM_FUNC_FLAG_CLASSMETHOD  = 1 << 1,  // @classmethod -> first arg is cls (the class)
    CBM_FUNC_FLAG_STATICMETHOD = 1 << 2,  // @staticmethod -> no implicit self/cls
    CBM_FUNC_FLAG_ABSTRACTMETHOD = 1 << 3, // @abstractmethod -> still callable for resolution
    CBM_FUNC_FLAG_OVERLOAD     = 1 << 4,  // @overload entry — non-implementation stub
    CBM_FUNC_FLAG_ASYNC        = 1 << 5,  // async def — return is Coroutine[..., T]
    CBM_FUNC_FLAG_GENERATOR    = 1 << 6,  // contains yield — return is Generator[T, ...]
    CBM_FUNC_FLAG_FINAL        = 1 << 7,  // @final — overrides not allowed
} CBMFuncFlags;

// Registered function/method with full type signature.
typedef struct {
    const char* qualified_name;  // e.g., "proj.pkg.TypeName.MethodName"
    const char* receiver_type;   // e.g., "proj.pkg.TypeName" (NULL for functions)
    const char* short_name;      // e.g., "MethodName"
    const CBMType* signature;    // FUNC type with param/return types
    const char** type_param_names; // NULL-terminated, e.g., ["T", "R", NULL] for generics
    int min_params;               // Minimum required params (excluding defaulted). -1 = unknown.
    int flags;                    // CBM_FUNC_FLAG_* bitfield (Python decorator info; 0 elsewhere)
    const char** decorator_qns;   // NULL-terminated decorator QNs (Python only); used for
                                   // user-decorator return-type substitution.
} CBMRegisteredFunc;

// Registered type with fields and method names.
typedef struct {
    const char* qualified_name;  // e.g., "proj.pkg.TypeName"
    const char* short_name;      // e.g., "TypeName"
    const char** field_names;    // NULL-terminated
    const CBMType** field_types; // NULL-terminated (parallel to field_names)
    const char** method_names;   // NULL-terminated (short names)
    const char** method_qns;     // NULL-terminated (qualified names, parallel)
    const char** embedded_types; // NULL-terminated (embedded/anonymous field type QNs)
    const char* alias_of;       // QN of aliased type (type Foo = Bar), NULL if not alias
    const char** type_param_names; // NULL-terminated, e.g., ["T", "K", NULL] for template classes
    bool is_interface;
} CBMRegisteredType;

// Cross-file type/function registry.
typedef struct {
    CBMRegisteredFunc* funcs;
    int func_count;
    int func_cap;

    CBMRegisteredType* types;
    int type_count;
    int type_cap;

    CBMArena* arena;  // owns all string data
} CBMTypeRegistry;

// Initialize a registry.
void cbm_registry_init(CBMTypeRegistry* reg, CBMArena* arena);

// Register a function/method.
void cbm_registry_add_func(CBMTypeRegistry* reg, CBMRegisteredFunc func);

// Register a type.
void cbm_registry_add_type(CBMTypeRegistry* reg, CBMRegisteredType type);

// Look up a method by receiver type QN + method name.
const CBMRegisteredFunc* cbm_registry_lookup_method(const CBMTypeRegistry* reg,
    const char* receiver_qn, const char* method_name);

// Look up a type by qualified name.
const CBMRegisteredType* cbm_registry_lookup_type(const CBMTypeRegistry* reg,
    const char* qualified_name);

// Look up a function by qualified name.
const CBMRegisteredFunc* cbm_registry_lookup_func(const CBMTypeRegistry* reg,
    const char* qualified_name);

// Look up a symbol (type or function) in a package by short name.
// package_qn is the package prefix (e.g., "proj.pkg").
const CBMRegisteredFunc* cbm_registry_lookup_symbol(const CBMTypeRegistry* reg,
    const char* package_qn, const char* name);

// Resolve type alias chain: follow alias_of until concrete type found (max 16 levels).
const CBMRegisteredType* cbm_registry_resolve_alias(const CBMTypeRegistry* reg, const char* type_qn);

// Look up a method by receiver type QN + method name, following alias chains.
const CBMRegisteredFunc* cbm_registry_lookup_method_aliased(const CBMTypeRegistry* reg,
    const char* receiver_qn, const char* method_name);

// Look up a method by receiver type + name, preferring the overload with matching arg count.
// Falls back to any match if no exact arg count match found.
const CBMRegisteredFunc* cbm_registry_lookup_method_by_args(const CBMTypeRegistry* reg,
    const char* receiver_qn, const char* method_name, int arg_count);

// Look up a free function by package + name, preferring matching arg count.
const CBMRegisteredFunc* cbm_registry_lookup_symbol_by_args(const CBMTypeRegistry* reg,
    const char* package_qn, const char* name, int arg_count);

// Look up a method by receiver type + name, scoring overloads by parameter type match.
// arg_types may contain NULL entries for unknown types. Falls back to arg-count matching.
const CBMRegisteredFunc* cbm_registry_lookup_method_by_types(const CBMTypeRegistry* reg,
    const char* receiver_qn, const char* method_name,
    const CBMType** arg_types, int arg_count);

// Look up a free function by package + name, scoring overloads by parameter type match.
const CBMRegisteredFunc* cbm_registry_lookup_symbol_by_types(const CBMTypeRegistry* reg,
    const char* package_qn, const char* name,
    const CBMType** arg_types, int arg_count);

#endif // CBM_LSP_TYPE_REGISTRY_H
