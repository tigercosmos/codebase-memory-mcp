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

/* ── Suite ─────────────────────────────────────────────────────── */

SUITE(py_lsp) {
    RUN_TEST(pylsp_smoke_empty);
    RUN_TEST(pylsp_smoke_one_function);
    RUN_TEST(pylsp_smoke_one_class);
    RUN_TEST(pylsp_no_crash_on_syntax_error);
    RUN_TEST(pylsp_smoke_imports_passed_through);
}
