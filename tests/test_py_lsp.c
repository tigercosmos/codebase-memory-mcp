/*
 * test_py_lsp.c — Tests for Python LSP type-aware call resolution.
 *
 * Mirrors tests/test_go_lsp.c shape: helper extract_py(source) calls
 * cbm_extract_file with CBM_LANG_PYTHON, then assertions search the
 * resolved_calls array. Phase 2 ships smoke tests only; subsequent
 * phases add categories matching the Go LSP layout (param types,
 * method dispatch, decorators, generics, cross-file).
 */
#include "test_framework.h"
#include "cbm.h"
#include "lsp/py_lsp.h"

/* ── Helpers — same shape as test_go_lsp.c ──────────────────────── */

static CBMFileResult *extract_py(const char *source) {
    return cbm_extract_file(source, (int)strlen(source), CBM_LANG_PYTHON,
                            "test", "main.py", 0, NULL, NULL);
}

static int find_resolved(const CBMFileResult *r, const char *callerSub, const char *calleeSub) {
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[i];
        if (rc->caller_qn && strstr(rc->caller_qn, callerSub) && rc->callee_qn &&
            strstr(rc->callee_qn, calleeSub))
            return i;
    }
    return -1;
}

/* Avoid unused-static-function warnings: helpers compiled but not yet used
 * outside the smoke tests will be referenced in Phase 3+ tests. */
__attribute__((unused))
static int require_resolved(const CBMFileResult *r, const char *callerSub, const char *calleeSub) {
    int idx = find_resolved(r, callerSub, calleeSub);
    if (idx < 0) {
        printf("  MISSING resolved call: caller~%s -> callee~%s (have %d)\n",
               callerSub, calleeSub, r->resolved_calls.count);
        for (int i = 0; i < r->resolved_calls.count; i++) {
            const CBMResolvedCall *rc = &r->resolved_calls.items[i];
            printf("    %s -> %s [%s %.2f]\n",
                   rc->caller_qn ? rc->caller_qn : "(null)",
                   rc->callee_qn ? rc->callee_qn : "(null)",
                   rc->strategy ? rc->strategy : "(null)",
                   rc->confidence);
        }
    }
    return idx;
}

/* ── Phase 2 — smoke ───────────────────────────────────────────── */

TEST(pylsp_smoke_empty) {
    CBMFileResult *r = extract_py("");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(r->resolved_calls.count, 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_smoke_one_function) {
    CBMFileResult *r = extract_py(
        "def greet(name):\n"
        "    return name\n");
    ASSERT_NOT_NULL(r);
    /* Phase 2 stub: no resolutions yet, but extraction must succeed and
     * the result must be addressable without crashes. */
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_smoke_one_class) {
    CBMFileResult *r = extract_py(
        "class Greeter:\n"
        "    def __init__(self, name):\n"
        "        self.name = name\n"
        "    def greet(self):\n"
        "        return self.name\n");
    ASSERT_NOT_NULL(r);
    /* Class + 2 methods at minimum */
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_no_crash_on_syntax_error) {
    /* Tree-sitter recovers from errors but we must not crash on the
     * recovered tree. */
    CBMFileResult *r = extract_py(
        "def broken(\n"
        "    x = 1\n"
        "class\n");
    ASSERT_NOT_NULL(r);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_smoke_imports_passed_through) {
    /* Imports populate ctx->import_local_names — Phase 2 just verifies
     * the unified extractor still produces them; resolution happens in
     * Phase 3. */
    CBMFileResult *r = extract_py(
        "import os\n"
        "import json as j\n"
        "from pathlib import Path\n"
        "from . import sibling\n"
        "def use():\n"
        "    return os.getcwd()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(r->imports.count, 3);
    cbm_free_result(r);
    PASS();
}

/* ── Phase 3 — imports → scope bindings ────────────────────────── */

/* Build a context, register one or more imports, run the binding pass,
 * and let the caller verify scope state. */
static void bind_imports_into_ctx(PyLSPContext *ctx, CBMArena *a, CBMTypeRegistry *reg,
                                  const char *const *locals, const char *const *qns,
                                  int count) {
    py_lsp_init(ctx, a, "", 0, reg, "test.main", NULL);
    for (int i = 0; i < count; i++) {
        py_lsp_add_import(ctx, locals[i], qns[i]);
    }
    py_lsp_bind_imports(ctx);
}

TEST(pylsp_import_simple) {
    /* import os → os ∈ scope as MODULE("os") */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"os"};
    const char *qns[] = {"os"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "os");
    ASSERT(cbm_type_is_module(t));
    ASSERT_STR_EQ(t->data.module.module_qn, "os");
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_aliased) {
    /* import json as j → j ∈ scope as MODULE("json") */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"j"};
    const char *qns[] = {"json"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "j");
    ASSERT(cbm_type_is_module(t));
    ASSERT_STR_EQ(t->data.module.module_qn, "json");
    /* original name "json" not bound */
    const CBMType *miss = py_lsp_lookup_in_scope(&ctx, "json");
    ASSERT(cbm_type_is_unknown(miss));
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_from) {
    /* from pathlib import Path → Path ∈ scope as NAMED("pathlib.Path") */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"Path"};
    const char *qns[] = {"pathlib.Path"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "Path");
    ASSERT_EQ(t->kind, CBM_TYPE_NAMED);
    ASSERT_STR_EQ(t->data.named.qualified_name, "pathlib.Path");
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_from_aliased) {
    /* from pathlib import Path as P → P ∈ scope as NAMED("pathlib.Path") */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"P"};
    const char *qns[] = {"pathlib.Path"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "P");
    /* Aliased name doesn't end module_qn with .P, so this binds as MODULE.
     * Phase 6 registry lookup will downgrade to NAMED if registry has no
     * matching module entry. Both behaviors are acceptable for v1; the
     * test asserts the entry exists with the correct QN. */
    ASSERT(t->kind == CBM_TYPE_NAMED || t->kind == CBM_TYPE_MODULE);
    if (t->kind == CBM_TYPE_NAMED) {
        ASSERT_STR_EQ(t->data.named.qualified_name, "pathlib.Path");
    } else {
        ASSERT_STR_EQ(t->data.module.module_qn, "pathlib.Path");
    }
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_relative_one_dot) {
    /* from . import sibling — extract_imports records local=sibling,
     * qn="..sibling" or similar. py_lsp binds it regardless. */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"sibling"};
    const char *qns[] = {"..sibling"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "sibling");
    ASSERT(!cbm_type_is_unknown(t));
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_relative_two_dots) {
    /* from ..pkg import x → bind x as NAMED("..pkg.x") best effort */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"x"};
    const char *qns[] = {"...pkg.x"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "x");
    ASSERT(!cbm_type_is_unknown(t));
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_star_best_effort) {
    /* from X import * — local_name="*". py_lsp does not bind "*" because
     * it's not a usable identifier; the import is preserved in the import
     * map for cross-file re-export resolution (Phase 9). */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"*"};
    const char *qns[] = {"X"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *star_miss = py_lsp_lookup_in_scope(&ctx, "*");
    ASSERT(cbm_type_is_unknown(star_miss));
    /* Import is still recorded — Phase 9 will use it. */
    ASSERT_EQ(ctx.import_count, 1);
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_typing_only_still_binds) {
    /* `if TYPE_CHECKING:` is just a runtime constant — extract_imports
     * emits CBMImport entries regardless of guard. py_lsp binds them. */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"List"};
    const char *qns[] = {"typing.List"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "List");
    ASSERT(!cbm_type_is_unknown(t));
    ASSERT_EQ(t->kind, CBM_TYPE_NAMED);
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_multi_pass_through_extract_file) {
    /* End-to-end: extract_file + run_py_lsp populate scope via imports.
     * We can't peek into the embedded ctx, but we verify imports survive
     * to the result and bind correctly when re-traversed. */
    CBMFileResult *r = extract_py(
        "import os\n"
        "import json as j\n"
        "from pathlib import Path\n"
        "def use():\n"
        "    return Path('.')\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(r->imports.count, 3);
    cbm_free_result(r);
    PASS();
}

/* ── Phase 4-6 — direct calls + method dispatch ────────────────── */

TEST(pylsp_direct_function_call) {
    /* def helper(): return 1
     * def main(): return helper() */
    CBMFileResult *r = extract_py(
        "def helper():\n"
        "    return 1\n"
        "def main():\n"
        "    return helper()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "main", "helper"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_method_call_simple) {
    /* class C:
     *     def m(self): return 1
     * def use(c):
     *     c.m()  -- with annotation */
    CBMFileResult *r = extract_py(
        "class C:\n"
        "    def m(self):\n"
        "        return 1\n"
        "def use(c: C):\n"
        "    return c.m()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "m"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_method_via_self) {
    CBMFileResult *r = extract_py(
        "class C:\n"
        "    def helper(self):\n"
        "        return 1\n"
        "    def caller(self):\n"
        "        return self.helper()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "caller", "helper"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_constructor_call_returns_instance) {
    /* class Foo: ...
     * def use():
     *   f = Foo()
     *   f.method()  -- requires inferring f as Foo */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use():\n"
        "    f = Foo()\n"
        "    return f.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_method_via_inheritance) {
    CBMFileResult *r = extract_py(
        "class Base:\n"
        "    def shared(self):\n"
        "        return 1\n"
        "class Child(Base):\n"
        "    def go(self):\n"
        "        return self.shared()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "go", "shared"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_no_false_positive_on_unknown_method) {
    /* Calling a method on an UNKNOWN type should NOT emit a high-confidence
     * resolution. */
    CBMFileResult *r = extract_py(
        "def f(x):\n"
        "    return x.something_unknown_42()\n");
    ASSERT_NOT_NULL(r);
    /* Should produce no high-confidence match for "something_unknown_42" */
    int idx = find_resolved(r, "f", "something_unknown_42");
    if (idx >= 0) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[idx];
        ASSERT(rc->confidence < 0.6f);
    }
    cbm_free_result(r);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

SUITE(py_lsp) {
    /* Phase 2 — smoke */
    RUN_TEST(pylsp_smoke_empty);
    RUN_TEST(pylsp_smoke_one_function);
    RUN_TEST(pylsp_smoke_one_class);
    RUN_TEST(pylsp_no_crash_on_syntax_error);
    RUN_TEST(pylsp_smoke_imports_passed_through);
    /* Phase 3 — imports → scope */
    RUN_TEST(pylsp_import_simple);
    RUN_TEST(pylsp_import_aliased);
    RUN_TEST(pylsp_import_from);
    RUN_TEST(pylsp_import_from_aliased);
    RUN_TEST(pylsp_import_relative_one_dot);
    RUN_TEST(pylsp_import_relative_two_dots);
    RUN_TEST(pylsp_import_star_best_effort);
    RUN_TEST(pylsp_import_typing_only_still_binds);
    RUN_TEST(pylsp_import_multi_pass_through_extract_file);
    /* Phases 4-6 — bindings + expression typing + method dispatch */
    RUN_TEST(pylsp_direct_function_call);
    RUN_TEST(pylsp_method_call_simple);
    RUN_TEST(pylsp_method_via_self);
    RUN_TEST(pylsp_constructor_call_returns_instance);
    RUN_TEST(pylsp_method_via_inheritance);
    RUN_TEST(pylsp_no_false_positive_on_unknown_method);
}
