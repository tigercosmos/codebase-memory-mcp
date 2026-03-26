#include "type_rep.h"
#include <stdint.h>
#include <string.h>

// Singleton UNKNOWN type (no allocation needed).
static const CBMType unknown_singleton = { .kind = CBM_TYPE_UNKNOWN };

const CBMType* cbm_type_unknown(void) {
    return &unknown_singleton;
}

const CBMType* cbm_type_named(CBMArena* a, const char* qualified_name) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_NAMED;
    t->data.named.qualified_name = cbm_arena_strdup(a, qualified_name);
    return t;
}

const CBMType* cbm_type_pointer(CBMArena* a, const CBMType* elem) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_POINTER;
    t->data.pointer.elem = elem;
    return t;
}

const CBMType* cbm_type_slice(CBMArena* a, const CBMType* elem) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_SLICE;
    t->data.slice.elem = elem;
    return t;
}

const CBMType* cbm_type_map(CBMArena* a, const CBMType* key, const CBMType* value) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_MAP;
    t->data.map.key = key;
    t->data.map.value = value;
    return t;
}

const CBMType* cbm_type_channel(CBMArena* a, const CBMType* elem, int direction) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_CHANNEL;
    t->data.channel.elem = elem;
    t->data.channel.direction = direction;
    return t;
}

const CBMType* cbm_type_func(CBMArena* a, const char** param_names,
                              const CBMType** param_types, const CBMType** return_types) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_FUNC;

    // Copy all arrays into arena memory to avoid dangling stack pointers.
    if (return_types) {
        int count = 0;
        while (return_types[count]) count++;
        const CBMType** arr = (const CBMType**)cbm_arena_alloc(a, (count + 1) * sizeof(const CBMType*));
        if (arr) {
            for (int i = 0; i < count; i++) arr[i] = return_types[i];
            arr[count] = NULL;
            t->data.func.return_types = arr;
        }
    }
    if (param_types) {
        int count = 0;
        while (param_types[count]) count++;
        const CBMType** arr = (const CBMType**)cbm_arena_alloc(a, (count + 1) * sizeof(const CBMType*));
        if (arr) {
            for (int i = 0; i < count; i++) arr[i] = param_types[i];
            arr[count] = NULL;
            t->data.func.param_types = arr;
        }
    }
    if (param_names) {
        int count = 0;
        while (param_names[count]) count++;
        const char** arr = (const char**)cbm_arena_alloc(a, (count + 1) * sizeof(const char*));
        if (arr) {
            for (int i = 0; i < count; i++) arr[i] = param_names[i];
            arr[count] = NULL;
            t->data.func.param_names = arr;
        }
    }
    return t;
}

const CBMType* cbm_type_builtin(CBMArena* a, const char* name) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_BUILTIN;
    t->data.builtin.name = cbm_arena_strdup(a, name);
    return t;
}

const CBMType* cbm_type_tuple(CBMArena* a, const CBMType** elems, int count) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_TUPLE;
    // Copy elems array
    const CBMType** arr = (const CBMType**)cbm_arena_alloc(a, (count + 1) * sizeof(const CBMType*));
    if (!arr) return &unknown_singleton;
    for (int i = 0; i < count; i++) arr[i] = elems[i];
    arr[count] = NULL;
    t->data.tuple.elems = arr;
    t->data.tuple.count = count;
    return t;
}

const CBMType* cbm_type_type_param(CBMArena* a, const char* name) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_TYPE_PARAM;
    t->data.type_param.name = cbm_arena_strdup(a, name);
    return t;
}

const CBMType* cbm_type_reference(CBMArena* a, const CBMType* elem) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_REFERENCE;
    t->data.reference.elem = elem;
    return t;
}

const CBMType* cbm_type_rvalue_ref(CBMArena* a, const CBMType* elem) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_RVALUE_REF;
    t->data.reference.elem = elem;
    return t;
}

const CBMType* cbm_type_template(CBMArena* a, const char* name, const CBMType** args, int arg_count) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_TEMPLATE;
    t->data.template_type.template_name = cbm_arena_strdup(a, name);
    if (args && arg_count > 0) {
        const CBMType** arr = (const CBMType**)cbm_arena_alloc(a, (arg_count + 1) * sizeof(const CBMType*));
        if (arr) {
            for (int i = 0; i < arg_count; i++) arr[i] = args[i];
            arr[arg_count] = NULL;
            t->data.template_type.template_args = arr;
        }
    }
    t->data.template_type.arg_count = arg_count;
    return t;
}

const CBMType* cbm_type_alias(CBMArena* a, const char* alias_qn, const CBMType* underlying) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_ALIAS;
    t->data.alias.alias_qn = cbm_arena_strdup(a, alias_qn);
    t->data.alias.underlying = underlying;
    return t;
}

// Operations

const CBMType* cbm_type_deref(const CBMType* t) {
    if (!t) return t;
    // Unwrap references transparently (C++ member access through refs)
    if (t->kind == CBM_TYPE_REFERENCE || t->kind == CBM_TYPE_RVALUE_REF)
        return t->data.reference.elem;
    if (t->kind != CBM_TYPE_POINTER) return t;
    return t->data.pointer.elem;
}

const CBMType* cbm_type_elem(const CBMType* t) {
    if (!t) return cbm_type_unknown();
    switch (t->kind) {
    case CBM_TYPE_POINTER:   return t->data.pointer.elem;
    case CBM_TYPE_SLICE:     return t->data.slice.elem;
    case CBM_TYPE_CHANNEL:   return t->data.channel.elem;
    case CBM_TYPE_REFERENCE: return t->data.reference.elem;
    case CBM_TYPE_RVALUE_REF: return t->data.reference.elem;
    default: return cbm_type_unknown();
    }
}

bool cbm_type_is_unknown(const CBMType* t) {
    if (!t) return true;
    /* Guard against dangling pointers from stale field_types entries.
     * Check alignment before dereferencing — misaligned pointer means garbage. */
    if (((uintptr_t)t & (_Alignof(CBMType) - 1)) != 0) return true;
    return t->kind == CBM_TYPE_UNKNOWN;
}

bool cbm_type_is_interface(const CBMType* t) {
    return t && t->kind == CBM_TYPE_INTERFACE;
}

bool cbm_type_is_pointer(const CBMType* t) {
    return t && t->kind == CBM_TYPE_POINTER;
}

bool cbm_type_is_reference(const CBMType* t) {
    return t && (t->kind == CBM_TYPE_REFERENCE || t->kind == CBM_TYPE_RVALUE_REF);
}

const CBMType* cbm_type_resolve_alias(const CBMType* t) {
    for (int i = 0; i < 16 && t; i++) {
        if (t->kind != CBM_TYPE_ALIAS) return t;
        if (!t->data.alias.underlying) return t;
        t = t->data.alias.underlying;
    }
    return t;
}

// Generic substitution: recursively replace TYPE_PARAM with concrete types.
const CBMType* cbm_type_substitute(CBMArena* a, const CBMType* t,
    const char** type_params, const CBMType** type_args) {
    if (!t) return cbm_type_unknown();
    if (!type_params || !type_args) return t;

    switch (t->kind) {
    case CBM_TYPE_TYPE_PARAM: {
        for (int i = 0; type_params[i]; i++) {
            if (strcmp(t->data.type_param.name, type_params[i]) == 0) {
                return type_args[i];
            }
        }
        return t; // unmatched param stays as-is
    }
    case CBM_TYPE_NAMED: {
        // Also substitute NAMED types matching template param names.
        // c_parse_return_type_text may parse "A" as NAMED("test.main.A")
        // instead of TYPE_PARAM("A") — check both full QN and short name.
        const char* qn = t->data.named.qualified_name;
        if (qn) {
            const char* short_name = strrchr(qn, '.');
            short_name = short_name ? short_name + 1 : qn;
            for (int i = 0; type_params[i]; i++) {
                if (strcmp(qn, type_params[i]) == 0 ||
                    strcmp(short_name, type_params[i]) == 0) {
                    return type_args[i];
                }
            }
        }
        return t;
    }
    case CBM_TYPE_POINTER:
        return cbm_type_pointer(a, cbm_type_substitute(a, t->data.pointer.elem, type_params, type_args));
    case CBM_TYPE_REFERENCE:
        return cbm_type_reference(a, cbm_type_substitute(a, t->data.reference.elem, type_params, type_args));
    case CBM_TYPE_RVALUE_REF:
        return cbm_type_rvalue_ref(a, cbm_type_substitute(a, t->data.reference.elem, type_params, type_args));
    case CBM_TYPE_SLICE:
        return cbm_type_slice(a, cbm_type_substitute(a, t->data.slice.elem, type_params, type_args));
    case CBM_TYPE_MAP:
        return cbm_type_map(a,
            cbm_type_substitute(a, t->data.map.key, type_params, type_args),
            cbm_type_substitute(a, t->data.map.value, type_params, type_args));
    case CBM_TYPE_CHANNEL:
        return cbm_type_channel(a, cbm_type_substitute(a, t->data.channel.elem, type_params, type_args), t->data.channel.direction);
    case CBM_TYPE_TUPLE: {
        int count = t->data.tuple.count;
        const CBMType** elems = (const CBMType**)cbm_arena_alloc(a, (count + 1) * sizeof(const CBMType*));
        if (!elems) return t;
        for (int i = 0; i < count; i++) {
            elems[i] = cbm_type_substitute(a, t->data.tuple.elems[i], type_params, type_args);
        }
        elems[count] = NULL;
        return cbm_type_tuple(a, elems, count);
    }
    default:
        return t; // NAMED, BUILTIN, FUNC, etc. — no type params to substitute
    }
}
