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

// --- Python-flavored constructors -------------------------------------------

// Dedupe members by structural equality, in place. Returns new length.
// Preserves first-seen order so output is deterministic.
static int union_member_dedupe(const CBMType** scratch, int count) {
    int out = 0;
    for (int i = 0; i < count; i++) {
        bool seen = false;
        for (int j = 0; j < out; j++) {
            if (cbm_type_equal(scratch[i], scratch[j])) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            scratch[out++] = scratch[i];
        }
    }
    return out;
}

const CBMType* cbm_type_union(CBMArena* a, const CBMType** members, int count) {
    if (!members || count <= 0) return &unknown_singleton;

    // Flatten: nested UNIONs unfold their members into the parent.
    int flat_cap = count * 2 + 4;
    const CBMType** flat = (const CBMType**)cbm_arena_alloc(a, flat_cap * sizeof(const CBMType*));
    if (!flat) return &unknown_singleton;
    int flat_count = 0;
    for (int i = 0; i < count; i++) {
        const CBMType* m = members[i];
        if (!m || cbm_type_is_unknown(m)) continue;
        if (m->kind == CBM_TYPE_UNION) {
            for (int j = 0; j < m->data.union_type.count; j++) {
                if (flat_count < flat_cap) flat[flat_count++] = m->data.union_type.members[j];
            }
        } else {
            if (flat_count < flat_cap) flat[flat_count++] = m;
        }
    }
    if (flat_count == 0) return &unknown_singleton;

    // Dedupe by structural equality.
    int unique_count = union_member_dedupe(flat, flat_count);
    if (unique_count == 1) return flat[0];

    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_UNION;
    const CBMType** arr = (const CBMType**)cbm_arena_alloc(a, (unique_count + 1) * sizeof(const CBMType*));
    if (!arr) return &unknown_singleton;
    for (int i = 0; i < unique_count; i++) arr[i] = flat[i];
    arr[unique_count] = NULL;
    t->data.union_type.members = arr;
    t->data.union_type.count = unique_count;
    return t;
}

const CBMType* cbm_type_optional(CBMArena* a, const CBMType* inner) {
    if (!inner) return &unknown_singleton;
    const CBMType* none_t = cbm_type_builtin(a, "None");
    const CBMType* members[2] = { inner, none_t };
    return cbm_type_union(a, members, 2);
}

const CBMType* cbm_type_literal(CBMArena* a, const CBMType* base, const char* literal_text) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_LITERAL;
    t->data.literal.base = base ? base : &unknown_singleton;
    t->data.literal.literal_text = literal_text ? cbm_arena_strdup(a, literal_text) : NULL;
    return t;
}

const CBMType* cbm_type_protocol(CBMArena* a, const char* qualified_name,
    const char** method_names, const CBMType** method_sigs) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_PROTOCOL;
    t->data.protocol.qualified_name = qualified_name ? cbm_arena_strdup(a, qualified_name) : NULL;

    int n = 0;
    if (method_names) {
        while (method_names[n]) n++;
    }
    if (n > 0) {
        const char** names = (const char**)cbm_arena_alloc(a, (n + 1) * sizeof(const char*));
        const CBMType** sigs = (const CBMType**)cbm_arena_alloc(a, (n + 1) * sizeof(const CBMType*));
        if (names && sigs) {
            for (int i = 0; i < n; i++) {
                names[i] = cbm_arena_strdup(a, method_names[i]);
                sigs[i] = method_sigs ? method_sigs[i] : NULL;
            }
            names[n] = NULL;
            sigs[n] = NULL;
            t->data.protocol.method_names = names;
            t->data.protocol.method_sigs = sigs;
        }
    }
    return t;
}

const CBMType* cbm_type_module(CBMArena* a, const char* module_qn) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_MODULE;
    t->data.module.module_qn = module_qn ? cbm_arena_strdup(a, module_qn) : NULL;
    return t;
}

const CBMType* cbm_type_callable(CBMArena* a, const CBMType** param_types, int param_count,
    const CBMType* return_type) {
    CBMType* t = (CBMType*)cbm_arena_alloc(a, sizeof(CBMType));
    if (!t) return &unknown_singleton;
    memset(t, 0, sizeof(CBMType));
    t->kind = CBM_TYPE_CALLABLE;
    t->data.callable.param_count = param_count;
    t->data.callable.return_type = return_type ? return_type : &unknown_singleton;
    if (param_count > 0 && param_types) {
        const CBMType** arr = (const CBMType**)cbm_arena_alloc(a, (param_count + 1) * sizeof(const CBMType*));
        if (arr) {
            for (int i = 0; i < param_count; i++) arr[i] = param_types[i];
            arr[param_count] = NULL;
            t->data.callable.param_types = arr;
        }
    }
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

bool cbm_type_is_union(const CBMType* t) {
    return t && t->kind == CBM_TYPE_UNION;
}

bool cbm_type_is_protocol(const CBMType* t) {
    return t && t->kind == CBM_TYPE_PROTOCOL;
}

bool cbm_type_is_module(const CBMType* t) {
    return t && t->kind == CBM_TYPE_MODULE;
}

static bool str_eq_or_both_null(const char* a, const char* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

bool cbm_type_equal(const CBMType* a, const CBMType* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
    case CBM_TYPE_UNKNOWN:
        return true;
    case CBM_TYPE_NAMED:
        return str_eq_or_both_null(a->data.named.qualified_name, b->data.named.qualified_name);
    case CBM_TYPE_BUILTIN:
        return str_eq_or_both_null(a->data.builtin.name, b->data.builtin.name);
    case CBM_TYPE_TYPE_PARAM:
        return str_eq_or_both_null(a->data.type_param.name, b->data.type_param.name);
    case CBM_TYPE_POINTER:
        return cbm_type_equal(a->data.pointer.elem, b->data.pointer.elem);
    case CBM_TYPE_SLICE:
        return cbm_type_equal(a->data.slice.elem, b->data.slice.elem);
    case CBM_TYPE_REFERENCE:
    case CBM_TYPE_RVALUE_REF:
        return cbm_type_equal(a->data.reference.elem, b->data.reference.elem);
    case CBM_TYPE_MAP:
        return cbm_type_equal(a->data.map.key, b->data.map.key)
            && cbm_type_equal(a->data.map.value, b->data.map.value);
    case CBM_TYPE_CHANNEL:
        return a->data.channel.direction == b->data.channel.direction
            && cbm_type_equal(a->data.channel.elem, b->data.channel.elem);
    case CBM_TYPE_TUPLE: {
        if (a->data.tuple.count != b->data.tuple.count) return false;
        for (int i = 0; i < a->data.tuple.count; i++) {
            if (!cbm_type_equal(a->data.tuple.elems[i], b->data.tuple.elems[i])) return false;
        }
        return true;
    }
    case CBM_TYPE_TEMPLATE: {
        if (!str_eq_or_both_null(a->data.template_type.template_name,
                                  b->data.template_type.template_name)) return false;
        if (a->data.template_type.arg_count != b->data.template_type.arg_count) return false;
        for (int i = 0; i < a->data.template_type.arg_count; i++) {
            if (!cbm_type_equal(a->data.template_type.template_args[i],
                                 b->data.template_type.template_args[i])) return false;
        }
        return true;
    }
    case CBM_TYPE_ALIAS:
        return str_eq_or_both_null(a->data.alias.alias_qn, b->data.alias.alias_qn);
    case CBM_TYPE_UNION: {
        if (a->data.union_type.count != b->data.union_type.count) return false;
        // Order-independent: every a-member must appear in b's set.
        for (int i = 0; i < a->data.union_type.count; i++) {
            bool found = false;
            for (int j = 0; j < b->data.union_type.count; j++) {
                if (cbm_type_equal(a->data.union_type.members[i],
                                    b->data.union_type.members[j])) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }
    case CBM_TYPE_LITERAL:
        return cbm_type_equal(a->data.literal.base, b->data.literal.base)
            && str_eq_or_both_null(a->data.literal.literal_text, b->data.literal.literal_text);
    case CBM_TYPE_PROTOCOL:
        return str_eq_or_both_null(a->data.protocol.qualified_name,
                                    b->data.protocol.qualified_name);
    case CBM_TYPE_MODULE:
        return str_eq_or_both_null(a->data.module.module_qn, b->data.module.module_qn);
    case CBM_TYPE_CALLABLE: {
        if (a->data.callable.param_count != b->data.callable.param_count) return false;
        if (!cbm_type_equal(a->data.callable.return_type, b->data.callable.return_type)) return false;
        if (a->data.callable.param_count > 0) {
            for (int i = 0; i < a->data.callable.param_count; i++) {
                if (!cbm_type_equal(a->data.callable.param_types[i],
                                     b->data.callable.param_types[i])) return false;
            }
        }
        return true;
    }
    case CBM_TYPE_FUNC:
    case CBM_TYPE_INTERFACE:
    case CBM_TYPE_STRUCT:
        // Structural equality on these is expensive and rarely needed by callers
        // beyond pointer identity (already checked above). Treat as not-equal.
        return false;
    }
    return false;
}

bool cbm_type_protocol_satisfied_by(const CBMType* proto, const CBMType* candidate) {
    if (!proto || proto->kind != CBM_TYPE_PROTOCOL) return false;
    if (!candidate) return false;
    // candidate must be a NAMED or PROTOCOL type with a method-name set we
    // can inspect. For PROTOCOL candidates, trivially satisfied if every
    // proto method appears in candidate's method list.
    if (candidate->kind == CBM_TYPE_PROTOCOL) {
        if (!proto->data.protocol.method_names) return true;
        for (int i = 0; proto->data.protocol.method_names[i]; i++) {
            const char* needed = proto->data.protocol.method_names[i];
            bool found = false;
            if (candidate->data.protocol.method_names) {
                for (int j = 0; candidate->data.protocol.method_names[j]; j++) {
                    if (str_eq_or_both_null(needed, candidate->data.protocol.method_names[j])) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found) return false;
        }
        return true;
    }
    // Nominal candidates require the registry — caller's responsibility.
    return false;
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
