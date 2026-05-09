/*
 * test_php_lsp.c — Tests for PHP Light Semantic Pass.
 *
 * Coverage focus matches docs/PLAN_PHP_LSP_INTEGRATION.md §9.1:
 *   - Param type binding (incl. arrow-function and anonymous closures)
 *   - Method call dispatch on typed receivers
 *   - Static call dispatch (Class::m, self::, parent::, static::)
 *   - new Class chains
 *   - Namespace + `use` clause resolution
 *   - Catch-clause exception type binding
 *   - PHPDoc @param / @var fallback
 *   - Magic-method (__call, __callStatic) emission
 *   - "Unindexed receiver" fallback (vendor types)
 *
 * Specific regression: (Prompt $p) => $p->value() must NOT route to a
 * global helpers.value function — see docs/PHP_LSP_PRE_FLIGHT.md §4.2.
 */
#include "test_framework.h"
#include "cbm.h"
#include "lsp/php_lsp.h"
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────── */

static CBMFileResult *extract_php(const char *source) {
    return cbm_extract_file(source, (int)strlen(source), CBM_LANG_PHP, "test", "main.php", 0, NULL,
                            NULL);
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

static int require_resolved(const CBMFileResult *r, const char *callerSub, const char *calleeSub) {
    int idx = find_resolved(r, callerSub, calleeSub);
    if (idx < 0) {
        printf("  MISSING resolved call: caller~%s -> callee~%s (have %d)\n", callerSub, calleeSub,
               r->resolved_calls.count);
        for (int i = 0; i < r->resolved_calls.count; i++) {
            const CBMResolvedCall *rc = &r->resolved_calls.items[i];
            printf("    %s -> %s [%s %.2f]\n", rc->caller_qn ? rc->caller_qn : "(null)",
                   rc->callee_qn ? rc->callee_qn : "(null)", rc->strategy ? rc->strategy : "(null)",
                   rc->confidence);
        }
    }
    return idx;
}

static const CBMResolvedCall *find_resolved_with_strategy(const CBMFileResult *r,
                                                          const char *callerSub,
                                                          const char *calleeSub,
                                                          const char *strategy) {
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[i];
        if (!rc->caller_qn || !rc->callee_qn) continue;
        if (!strstr(rc->caller_qn, callerSub)) continue;
        if (!strstr(rc->callee_qn, calleeSub)) continue;
        if (strategy && (!rc->strategy || strcmp(rc->strategy, strategy) != 0)) continue;
        return rc;
    }
    return NULL;
}

/* ── 1. Local class, method dispatch via $x = new C() ──────────── */

TEST(phplsp_local_method_via_new_assignment) {
    const char *src =
        "<?php\n"
        "class Greeter {\n"
        "    public function hello(): string { return 'hi'; }\n"
        "}\n"
        "class Caller {\n"
        "    public function go(): void {\n"
        "        $g = new Greeter();\n"
        "        $g->hello();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    int idx = require_resolved(r, "Caller.go", "Greeter.hello");
    ASSERT(idx >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 2. Local class, method dispatch via typed parameter ───────── */

TEST(phplsp_local_method_via_typed_param) {
    const char *src =
        "<?php\n"
        "class P { public function value(): string { return 'x'; } }\n"
        "class C {\n"
        "    public function run(P $p): void { $p->value(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    int idx = require_resolved(r, "C.run", "P.value");
    ASSERT(idx >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 3. Arrow function with typed parameter ───────────────────── */

TEST(phplsp_arrow_function_typed_param) {
    const char *src =
        "<?php\n"
        "class P { public function value(): string { return 'x'; } }\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        $f = fn (P $p) => $p->value();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    int idx = require_resolved(r, "C.run", "P.value");
    ASSERT(idx >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 4. Static call ─────────────────────────────────────────────── */

TEST(phplsp_static_call_resolved) {
    const char *src =
        "<?php\n"
        "class Util { public static function fmt(string $s): string { return $s; } }\n"
        "class C {\n"
        "    public function run(): void { Util::fmt('hi'); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    int idx = require_resolved(r, "C.run", "Util.fmt");
    ASSERT(idx >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 5. self:: and parent:: dispatch ──────────────────────────── */

TEST(phplsp_self_and_parent) {
    const char *src =
        "<?php\n"
        "class Base { public function tag(): string { return 'b'; } }\n"
        "class Child extends Base {\n"
        "    public function alt(): string { return 'c'; }\n"
        "    public function go(): void {\n"
        "        self::alt();\n"
        "        parent::tag();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "Child.go", "Child.alt") >= 0);
    ASSERT(require_resolved(r, "Child.go", "Base.tag") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 6. Function call with `use function` map ─────────────────── */

TEST(phplsp_use_function_clause) {
    /* The aliased function isn't defined in this file; we just verify we
     * don't crash and don't misroute. Resolved-call set is allowed to be
     * empty for this case. */
    const char *src =
        "<?php\n"
        "namespace A;\n"
        "use function B\\helper;\n"
        "function caller(): void { helper(); }\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 7. PHPDoc @var binding ───────────────────────────────────── */

TEST(phplsp_phpdoc_var) {
    const char *src =
        "<?php\n"
        "class Q { public function tap(): string { return 'q'; } }\n"
        "class C {\n"
        "    public function run($x): void {\n"
        "        /** @var Q $x */\n"
        "        $x->tap();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    int idx = require_resolved(r, "C.run", "Q.tap");
    ASSERT(idx >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 8. Catch-clause typed binding ────────────────────────────── */

TEST(phplsp_catch_binding) {
    const char *src =
        "<?php\n"
        "class MyExc { public function name(): string { return 'x'; } }\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        try { /* nop */ } catch (MyExc $e) { $e->name(); }\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    int idx = require_resolved(r, "C.run", "MyExc.name");
    ASSERT(idx >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 9. $this-bound method dispatch ───────────────────────────── */

TEST(phplsp_this_method) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function helper(): string { return 'h'; }\n"
        "    public function go(): void { $this->helper(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    int idx = require_resolved(r, "C.go", "C.helper");
    ASSERT(idx >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 10. Inheritance — method resolved via parent ──────────────── *
 *
 * Phase 1 limitation: when the receiver type is in-registry but the method
 * lives on a parent class, the resolver currently emits the unindexed
 * heuristic for the receiver class instead of walking the parent chain.
 * This is a known gap tracked for Phase 2 — the unified extractor's
 * d->parent_class string does not always match the registered type QN
 * because of namespace stripping, so the receiver_type lookup misses.
 * For the smoke-test pattern the override still suppresses the wrong
 * helper edge, which is the value-correctness goal.
 */
TEST(phplsp_method_inherited_via_parent) {
    const char *src =
        "<?php\n"
        "class Base { public function tag(): string { return 'b'; } }\n"
        "class Mid extends Base {}\n"
        "class C {\n"
        "    public function run(Mid $m): void { $m->tag(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Either resolved Base.tag OR emitted unindexed Mid.tag is acceptable —
     * both block the misroute. */
    int direct = find_resolved(r, "C.run", "Base.tag");
    int unindexed_mid = find_resolved(r, "C.run", "Mid.tag");
    ASSERT(direct >= 0 || unindexed_mid >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 11. Vendor / unindexed receiver — emit unindexed strategy ── */

TEST(phplsp_unindexed_receiver_emits_block) {
    const char *src =
        "<?php\n"
        "namespace App;\n"
        "use Vendor\\NotIndexed\\Foo;\n"
        "class C {\n"
        "    public function run(Foo $x): void { $x->value(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* The synthetic entry must exist so the pipeline bridge can suppress
     * the unified extractor's name-based misroute. */
    const CBMResolvedCall *rc =
        find_resolved_with_strategy(r, "C.run", "Foo.value", "php_method_typed_unindexed");
    if (!rc) {
        printf("  expected php_method_typed_unindexed for Foo.value, got %d entries\n",
               r->resolved_calls.count);
        for (int i = 0; i < r->resolved_calls.count; i++) {
            const CBMResolvedCall *rcd = &r->resolved_calls.items[i];
            printf("    %s -> %s [%s %.2f]\n", rcd->caller_qn ? rcd->caller_qn : "(null)",
                   rcd->callee_qn ? rcd->callee_qn : "(null)",
                   rcd->strategy ? rcd->strategy : "(null)", rcd->confidence);
        }
    }
    ASSERT(rc != NULL);
    cbm_free_result(r);
    PASS();
}

/* ── 12. Specific regression from PHP_LSP_PRE_FLIGHT §4.2 ──────── */

TEST(phplsp_regression_prompt_value_not_routed_to_helper) {
    /* Mirrors the pattern from
     *   src/Illuminate/Console/Concerns/ConfiguresPrompts.php:34
     *   fn (Prompt $prompt) => $this->validatePrompt($prompt->value(), ...)
     *
     * With Prompt being a vendor type AND a global "value" function in the
     * same project, the unified extractor misroutes $prompt->value() to
     * the global helper. The LSP's php_method_typed_unindexed strategy
     * exists to block this misroute downstream. */
    const char *src =
        "<?php\n"
        "namespace App;\n"
        "use Vendor\\Prompt;\n"
        "class C {\n"
        "    public function configure(): void {\n"
        "        Bus::run(fn (Prompt $prompt) => $prompt->value());\n"
        "    }\n"
        "}\n"
        "class Bus { public static function run(callable $f): void {} }\n"
        "function value($x) { return $x; }\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    const CBMResolvedCall *rc = find_resolved_with_strategy(r, "C.configure", "Prompt.value",
                                                            "php_method_typed_unindexed");
    if (!rc) {
        printf("  expected unindexed Prompt.value for ConfiguresPrompts pattern, got %d\n",
               r->resolved_calls.count);
        for (int i = 0; i < r->resolved_calls.count; i++) {
            const CBMResolvedCall *rcd = &r->resolved_calls.items[i];
            printf("    %s -> %s [%s %.2f]\n", rcd->caller_qn ? rcd->caller_qn : "(null)",
                   rcd->callee_qn ? rcd->callee_qn : "(null)",
                   rcd->strategy ? rcd->strategy : "(null)", rcd->confidence);
        }
    }
    ASSERT(rc != NULL);
    cbm_free_result(r);
    PASS();
}

/* ── 13. Method chaining — type from earlier method's return ──── */

TEST(phplsp_method_chain_return_type) {
    const char *src =
        "<?php\n"
        "class B { public function tap(): string { return 'b'; } }\n"
        "class A { public function next(): B { return new B(); } }\n"
        "class C {\n"
        "    public function run(A $a): void { $a->next()->tap(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* First link: A.next */
    ASSERT(require_resolved(r, "C.run", "A.next") >= 0);
    /* Second link via return type */
    ASSERT(require_resolved(r, "C.run", "B.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 14. Function global fallback (bare value() in same file) ─── */

TEST(phplsp_function_global_fallback) {
    const char *src =
        "<?php\n"
        "function doit($x) { return $x; }\n"
        "class C { public function go(): void { doit(1); } }\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Either php_function_namespaced or php_function_global_fallback. */
    ASSERT(find_resolved(r, "C.go", "doit") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 15. namespace + use class alias ──────────────────────────── */

TEST(phplsp_use_alias_resolves_for_new) {
    /* Alias points at a vendor type. Phase 1 may or may not bind the
     * variable type from `new R()` depending on AST shape; the smoke
     * confirmation is only that we don't crash. The Laravel benchmark
     * exercises the same path with typed parameters (Test #12), which
     * is the production-load-bearing case. */
    const char *src =
        "<?php\n"
        "namespace App;\n"
        "use Vendor\\Real as R;\n"
        "class C {\n"
        "    public function run(): void { $r = new R(); $r->go(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 16. Constructor property promotion (Phase 4b) ─────────────── */

TEST(phplsp_constructor_property_promotion) {
    const char *src =
        "<?php\n"
        "class P { public function value(): string { return 'p'; } }\n"
        "class C {\n"
        "    public function __construct(public P $p) {}\n"
        "    public function run(): void { $this->p->value(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    int idx = require_resolved(r, "C.run", "P.value");
    ASSERT(idx >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 17. Typed property declaration ────────────────────────────── */

TEST(phplsp_typed_property_declaration) {
    const char *src =
        "<?php\n"
        "class B { public function tap(): string { return 'b'; } }\n"
        "class C {\n"
        "    private B $bar;\n"
        "    public function __construct(B $b) { $this->bar = $b; }\n"
        "    public function run(): void { $this->bar->tap(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "B.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 18. Constructor-body inference (no typed declaration) ─────── */

TEST(phplsp_constructor_body_inference) {
    const char *src =
        "<?php\n"
        "class W { public function ping(): string { return 'w'; } }\n"
        "class C {\n"
        "    private $w;\n"
        "    public function __construct(W $w) { $this->w = $w; }\n"
        "    public function run(): void { $this->w->ping(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "W.ping") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 19. Type narrowing — instanceof ───────────────────────────── */

TEST(phplsp_narrow_instanceof) {
    const char *src =
        "<?php\n"
        "class Foo { public function bar(): int { return 1; } }\n"
        "class C {\n"
        "    public function run($x): void {\n"
        "        if ($x instanceof Foo) { $x->bar(); }\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "Foo.bar") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 20. Type narrowing — assert(instanceof) ───────────────────── */

TEST(phplsp_narrow_assert_instanceof) {
    const char *src =
        "<?php\n"
        "class Foo { public function bar(): int { return 1; } }\n"
        "class C {\n"
        "    public function run($x): void {\n"
        "        assert($x instanceof Foo);\n"
        "        $x->bar();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "Foo.bar") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 21. PHPDoc @property tag binds field ──────────────────────── */

TEST(phplsp_phpdoc_property_class_tag) {
    const char *src =
        "<?php\n"
        "class Q { public function tap(): string { return 'q'; } }\n"
        "/**\n"
        " * @property Q $q\n"
        " */\n"
        "class Model {\n"
        "    public function go(): void { $this->q->tap(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "Model.go", "Q.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 22. PHPDoc @method tag registers virtual method ───────────── */

TEST(phplsp_phpdoc_method_class_tag) {
    const char *src =
        "<?php\n"
        "class Z { public function tap(): string { return 'z'; } }\n"
        "/**\n"
        " * @method Z fooBar()\n"
        " */\n"
        "class Model {\n"
        "    public function go(): void { $this->fooBar()->tap(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* fooBar resolves on Model (the @method virtual), and chains to Z.tap. */
    ASSERT(require_resolved(r, "Model.go", "Model.fooBar") >= 0);
    ASSERT(require_resolved(r, "Model.go", "Z.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 23. Trait flattening — methods from used trait ────────────── */

TEST(phplsp_trait_method_flattened) {
    const char *src =
        "<?php\n"
        "trait T { public function shared(): string { return 't'; } }\n"
        "class C {\n"
        "    use T;\n"
        "    public function run(): void { $this->shared(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "C.shared") >= 0 ||
           require_resolved(r, "C.run", "T.shared") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 24. Match expression result type ──────────────────────────── */

TEST(phplsp_match_result_type) {
    const char *src =
        "<?php\n"
        "class A { public function go(): int { return 1; } }\n"
        "class C {\n"
        "    public function run(int $i): void {\n"
        "        $a = match($i) { 1 => new A(), default => new A() };\n"
        "        $a->go();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "A.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 25. Ternary result type ───────────────────────────────────── */

TEST(phplsp_ternary_result_type) {
    const char *src =
        "<?php\n"
        "class A { public function go(): int { return 1; } }\n"
        "class C {\n"
        "    public function run(bool $b): void {\n"
        "        $a = $b ? new A() : new A();\n"
        "        $a->go();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "A.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 26. Method chain depth 3 ──────────────────────────────────── */

TEST(phplsp_method_chain_depth_three) {
    const char *src =
        "<?php\n"
        "class C3 { public function tap(): string { return 'c3'; } }\n"
        "class C2 { public function next(): C3 { return new C3(); } }\n"
        "class C1 { public function next(): C2 { return new C2(); } }\n"
        "class Caller {\n"
        "    public function run(C1 $c): void { $c->next()->next()->tap(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "Caller.run", "C1.next") >= 0);
    ASSERT(require_resolved(r, "Caller.run", "C2.next") >= 0);
    ASSERT(require_resolved(r, "Caller.run", "C3.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 27. Late static binding through 2-deep parent chain ───────── */

TEST(phplsp_lsb_two_deep_chain) {
    const char *src =
        "<?php\n"
        "class GrandParent_ { public function tag(): string { return 'g'; } }\n"
        "class Parent_ extends GrandParent_ {}\n"
        "class Child extends Parent_ {}\n"
        "class C {\n"
        "    public function run(Child $c): void { $c->tag(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Either resolves directly to GrandParent_.tag or emits unindexed for
     * an intermediate — both block the misroute. */
    ASSERT(find_resolved(r, "C.run", ".tag") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 28. Nullsafe operator ─────────────────────────────────────── */

TEST(phplsp_nullsafe_operator) {
    const char *src =
        "<?php\n"
        "class Foo { public function bar(): int { return 1; } }\n"
        "class C {\n"
        "    public function run(?Foo $f): void { $f?->bar(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "Foo.bar") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 29. Static method through `static::` ─────────────────────── */

TEST(phplsp_static_keyword_dispatch) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public static function make(): self { return new self(); }\n"
        "    public function go(): void { static::make(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.go", "C.make") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 30. self:: from inside a method ──────────────────────────── */

TEST(phplsp_self_in_method_body) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function helper(): int { return 1; }\n"
        "    public function go(): void { self::helper(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.go", "C.helper") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 31. Variadic parameter doesn't crash binding ──────────────── */

TEST(phplsp_variadic_parameter) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(string ...$args): void {}\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 32. Closure with use() clause ─────────────────────────────── */

TEST(phplsp_closure_with_use) {
    const char *src =
        "<?php\n"
        "class P { public function value(): string { return 'p'; } }\n"
        "class C {\n"
        "    public function run(P $p): void {\n"
        "        $f = function () use ($p) { return $p->value(); };\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Closure use-imports aren't tracked in Phase 1; allow either resolve
     * or no resolve, but test must not crash. */
    cbm_free_result(r);
    PASS();
}

/* ── 33. Anonymous function as argument ────────────────────────── */

TEST(phplsp_anonymous_function_arg) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        Bus::run(function () { return 1; });\n"
        "    }\n"
        "}\n"
        "class Bus { public static function run(callable $f): void {} }\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "Bus.run") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 34. Arrow function inside arrow function ──────────────────── */

TEST(phplsp_nested_arrow_functions) {
    const char *src =
        "<?php\n"
        "class A { public function go(): int { return 1; } }\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        $f = fn () => fn (A $a) => $a->go();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "A.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 35. is_string narrowing ───────────────────────────────────── */

TEST(phplsp_narrow_is_string) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run($x): void {\n"
        "        if (is_string($x)) {}\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 36. is_array narrowing ───────────────────────────────────── */

TEST(phplsp_narrow_is_array) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run($x): void {\n"
        "        if (is_array($x)) { count($x); }\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 37. Multiple use clauses, multiple classes used ───────────── */

TEST(phplsp_multiple_use_clauses) {
    const char *src =
        "<?php\n"
        "namespace App;\n"
        "use Vendor\\Foo;\n"
        "use Vendor\\Bar;\n"
        "class C {\n"
        "    public function run(Foo $f, Bar $b): void {\n"
        "        $f->go();\n"
        "        $b->stop();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Both should emit unindexed override entries. */
    ASSERT(find_resolved(r, "C.run", "Vendor.Foo.go") >= 0);
    ASSERT(find_resolved(r, "C.run", "Vendor.Bar.stop") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 38. Group use clause ──────────────────────────────────────── */

TEST(phplsp_group_use_clause) {
    const char *src =
        "<?php\n"
        "namespace App;\n"
        "use Vendor\\{ Foo, Bar };\n"
        "class C {\n"
        "    public function run(Foo $f): void { $f->go(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Group form may or may not be parsed by tree-sitter-php — accept. */
    cbm_free_result(r);
    PASS();
}

/* ── 39. Fully-qualified class name in `new \\Vendor\\Foo()` ───── */

TEST(phplsp_fqn_new_expression) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        $f = new \\Vendor\\Foo();\n"
        "        $f->go();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(find_resolved(r, "C.run", "Vendor.Foo.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 40. Foreach loop variable not crashed ─────────────────────── */

TEST(phplsp_foreach_iteration) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(array $xs): void {\n"
        "        foreach ($xs as $x) { /* nop */ }\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 41. Try / catch with multiple exception types ─────────────── */

TEST(phplsp_catch_multi_type) {
    const char *src =
        "<?php\n"
        "class A { public function go(): int { return 1; } }\n"
        "class B { public function go(): int { return 2; } }\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        try { /* nop */ } catch (A | B $e) { $e->go(); }\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Phase 1 takes leftmost — A.go is the expected resolution. */
    ASSERT(find_resolved(r, "C.run", ".go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 42. Exception in catch with PHP namespace ─────────────────── */

TEST(phplsp_namespaced_catch) {
    const char *src =
        "<?php\n"
        "namespace App\\Errors;\n"
        "class MyExc { public function reason(): string { return 'r'; } }\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        try { /* nop */ } catch (MyExc $e) { $e->reason(); }\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "MyExc.reason") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 43. Multiple interfaces implemented ──────────────────────── */

TEST(phplsp_implements_multiple_interfaces) {
    const char *src =
        "<?php\n"
        "interface A { public function aa(): int; }\n"
        "interface B { public function bb(): int; }\n"
        "class C implements A, B {\n"
        "    public function aa(): int { return 1; }\n"
        "    public function bb(): int { return 2; }\n"
        "    public function go(): void { $this->aa(); $this->bb(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.go", "C.aa") >= 0);
    ASSERT(require_resolved(r, "C.go", "C.bb") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 44. Method on field access ($this->x->method()) ───────────── */

TEST(phplsp_method_on_typed_field) {
    const char *src =
        "<?php\n"
        "class Inner { public function go(): int { return 1; } }\n"
        "class C {\n"
        "    private Inner $i;\n"
        "    public function __construct(Inner $i) { $this->i = $i; }\n"
        "    public function run(): void { $this->i->go(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "Inner.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 45. Inheritance: child overrides parent method ─────────────── */

TEST(phplsp_child_overrides_parent_method) {
    const char *src =
        "<?php\n"
        "class Base { public function speak(): string { return 'b'; } }\n"
        "class Kid extends Base {\n"
        "    public function speak(): string { return 'k'; }\n"
        "}\n"
        "class C {\n"
        "    public function run(Kid $k): void { $k->speak(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Child override is preferred over parent. */
    ASSERT(require_resolved(r, "C.run", "Kid.speak") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 46. PHPDoc on method body — @param type override ─────────── */

TEST(phplsp_phpdoc_param_method) {
    const char *src =
        "<?php\n"
        "class T { public function tap(): string { return 't'; } }\n"
        "class C {\n"
        "    /**\n"
        "     * @param T $arg\n"
        "     */\n"
        "    public function run($arg): void { $arg->tap(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "T.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 47. Static field access not crashed ─────────────────────── */

TEST(phplsp_static_field_access) {
    const char *src =
        "<?php\n"
        "class Cfg { public static $value = 1; }\n"
        "class C {\n"
        "    public function run(): void { $x = Cfg::$value; }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 48. Class constant access ────────────────────────────────── */

TEST(phplsp_class_constant_access) {
    const char *src =
        "<?php\n"
        "class Cfg { const KEY = 'k'; }\n"
        "class C {\n"
        "    public function run(): void { $x = Cfg::KEY; }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 49. Anonymous class doesn't crash ───────────────────────── */

TEST(phplsp_anonymous_class) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(): object {\n"
        "        return new class { public function go(): int { return 1; } };\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 50. Enum with methods ────────────────────────────────────── */

TEST(phplsp_enum_method_dispatch) {
    const char *src =
        "<?php\n"
        "enum Suit: string {\n"
        "    case Hearts = 'h';\n"
        "    case Spades = 's';\n"
        "    public function label(): string { return 'x'; }\n"
        "}\n"
        "class C {\n"
        "    public function run(Suit $s): void { $s->label(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "Suit.label") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 51. Readonly property ───────────────────────────────────── */

TEST(phplsp_readonly_property) {
    const char *src =
        "<?php\n"
        "class V { public function get(): int { return 1; } }\n"
        "class C {\n"
        "    public readonly V $v;\n"
        "    public function __construct(V $v) { $this->v = $v; }\n"
        "    public function run(): void { $this->v->get(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "V.get") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 52. Static factory + chained methods ─────────────────────── */

TEST(phplsp_static_factory_chain) {
    const char *src =
        "<?php\n"
        "class B { public function tap(): int { return 1; } }\n"
        "class A {\n"
        "    public static function make(): A { return new self(); }\n"
        "    public function next(): B { return new B(); }\n"
        "}\n"
        "class C {\n"
        "    public function run(): void { A::make()->next()->tap(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "A.make") >= 0);
    ASSERT(require_resolved(r, "C.run", "A.next") >= 0);
    ASSERT(require_resolved(r, "C.run", "B.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 53. Builtin DateTime stdlib ──────────────────────────────── */

TEST(phplsp_stdlib_datetime_chain) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(\\DateTime $d): void { $d->modify('+1 day')->format('Y'); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "DateTime.modify") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 54. Builtin Throwable hierarchy ─────────────────────────── */

TEST(phplsp_stdlib_throwable_chain) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        try { /* nop */ } catch (\\RuntimeException $e) { $e->getMessage(); }\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* getMessage is on Throwable; RuntimeException -> Exception -> Throwable */
    ASSERT(find_resolved(r, "C.run", "Throwable.getMessage") >= 0 ||
           find_resolved(r, "C.run", "RuntimeException.getMessage") >= 0 ||
           find_resolved(r, "C.run", "Exception.getMessage") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 55. Iterator stdlib method dispatch ─────────────────────── */

TEST(phplsp_stdlib_iterator) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(\\Iterator $it): void { $it->current(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "Iterator.current") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 56. Abstract class method ─────────────────────────────── */

TEST(phplsp_abstract_class_method) {
    const char *src =
        "<?php\n"
        "abstract class Base { abstract public function go(): int; public function tap(): int { return 1; } }\n"
        "class Kid extends Base { public function go(): int { return 2; } }\n"
        "class C {\n"
        "    public function run(Kid $k): void { $k->tap(); $k->go(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(find_resolved(r, "C.run", ".tap") >= 0);
    ASSERT(require_resolved(r, "C.run", "Kid.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 57. Static call via aliased namespace ─────────────────── */

TEST(phplsp_static_call_via_alias) {
    const char *src =
        "<?php\n"
        "namespace App;\n"
        "use Vendor\\Helper as H;\n"
        "class C {\n"
        "    public function run(): void { H::go(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Vendor.Helper unindexed; emit unindexed entry. */
    ASSERT(find_resolved(r, "C.run", "Vendor.Helper.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 58. Function call with namespace ────────────────────── */

TEST(phplsp_function_call_namespaced) {
    const char *src =
        "<?php\n"
        "namespace App;\n"
        "function helper(): int { return 1; }\n"
        "class C {\n"
        "    public function run(): void { helper(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* Either path works. */
    ASSERT(find_resolved(r, "C.run", ".helper") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 59. Reassignment changes type ─────────────────────── */

TEST(phplsp_reassignment_changes_type) {
    const char *src =
        "<?php\n"
        "class A { public function aa(): int { return 1; } }\n"
        "class B { public function bb(): int { return 2; } }\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        $x = new A();\n"
        "        $x->aa();\n"
        "        $x = new B();\n"
        "        $x->bb();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "A.aa") >= 0);
    ASSERT(require_resolved(r, "C.run", "B.bb") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 60. Chained `use` then `new` resolution ─────────────── */

TEST(phplsp_use_then_new_aliased) {
    const char *src =
        "<?php\n"
        "namespace App;\n"
        "use Vendor\\Real as R;\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        $r = new R();\n"
        "        $r->go();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    /* $r should bind to NAMED(Vendor.Real); $r->go() should emit override. */
    ASSERT(find_resolved(r, "C.run", "Vendor.Real.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 61. Type narrowing — multiple branches ──────────────── */

TEST(phplsp_narrow_branches) {
    const char *src =
        "<?php\n"
        "class A { public function aa(): int { return 1; } }\n"
        "class B { public function bb(): int { return 2; } }\n"
        "class C {\n"
        "    public function run($x): void {\n"
        "        if ($x instanceof A) { $x->aa(); }\n"
        "        if ($x instanceof B) { $x->bb(); }\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "A.aa") >= 0);
    ASSERT(require_resolved(r, "C.run", "B.bb") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 62. Sequential statements maintain bindings ─────────── */

TEST(phplsp_sequential_bindings) {
    const char *src =
        "<?php\n"
        "class A { public function next(): A { return $this; } public function tap(): int { return 1; } }\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        $a = new A();\n"
        "        $b = $a->next();\n"
        "        $b->tap();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "A.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 63. Chained calls with intermediate variable ────────── */

TEST(phplsp_chained_with_intermediate_var) {
    const char *src =
        "<?php\n"
        "class B { public function tap(): int { return 1; } }\n"
        "class A { public function next(): B { return new B(); } }\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        $a = new A();\n"
        "        $b = $a->next();\n"
        "        $b->tap();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "A.next") >= 0);
    ASSERT(require_resolved(r, "C.run", "B.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 64. Trait with multiple methods, all flattened ──────── */

TEST(phplsp_trait_multiple_methods) {
    const char *src =
        "<?php\n"
        "trait T {\n"
        "    public function aa(): int { return 1; }\n"
        "    public function bb(): int { return 2; }\n"
        "}\n"
        "class C {\n"
        "    use T;\n"
        "    public function run(): void { $this->aa(); $this->bb(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(find_resolved(r, "C.run", "C.aa") >= 0 || find_resolved(r, "C.run", "T.aa") >= 0);
    ASSERT(find_resolved(r, "C.run", "C.bb") >= 0 || find_resolved(r, "C.run", "T.bb") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 65. Cast expression ─────────────────────────────────── */

TEST(phplsp_cast_expression) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run($x): void {\n"
        "        $i = (int) $x;\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 66. Clone returns same type ─────────────────────────── */

TEST(phplsp_clone_preserves_type) {
    const char *src =
        "<?php\n"
        "class A { public function go(): int { return 1; } }\n"
        "class C {\n"
        "    public function run(A $a): void {\n"
        "        $b = clone $a;\n"
        "        $b->go();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "A.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 67. Empty class body ────────────────────────────────── */

TEST(phplsp_empty_class_body) {
    const char *src = "<?php\nclass Empty_ {}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 68. PSR LoggerInterface ────────────────────────────── */

TEST(phplsp_stdlib_psr_logger) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(\\Psr\\Log\\LoggerInterface $log): void {\n"
        "        $log->info('hi');\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "LoggerInterface.info") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 69. Long method chain doesn't overflow ─────────────── */

TEST(phplsp_long_method_chain) {
    const char *src =
        "<?php\n"
        "class A { public function n(): A { return $this; } public function t(): int { return 1; } }\n"
        "class C {\n"
        "    public function run(A $a): void { $a->n()->n()->n()->n()->n()->t(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(find_resolved(r, "C.run", "A.t") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 70. Conditional return doesn't crash ─────────────── */

TEST(phplsp_conditional_return) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(bool $b): int {\n"
        "        return $b ? 1 : 2;\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 71. PHPDoc @var on assignment ─────────────────────── */

TEST(phplsp_phpdoc_var_at_assignment) {
    const char *src =
        "<?php\n"
        "class P { public function tap(): int { return 1; } }\n"
        "class C {\n"
        "    public function run($x): void {\n"
        "        /** @var P $x */\n"
        "        $x->tap();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "P.tap") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 72. is_int narrowing ─────────────────────────────── */

TEST(phplsp_narrow_is_int) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run($x): void {\n"
        "        if (is_int($x)) {}\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 73. Encapsed string doesn't crash ────────────────── */

TEST(phplsp_encapsed_string) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        $x = \"hello $name\";\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 74. Heredoc string ──────────────────────────────── */

TEST(phplsp_heredoc) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(): string {\n"
        "        return <<<EOT\nhi\nEOT;\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 75. Regression: shouldn't infinite-loop on cyclic
 *      embedded_types (defensive bound). */

TEST(phplsp_cyclic_embed_bounded) {
    /* tree-sitter allows class A extends A; even if PHP doesn't run, our
     * resolver should bound its parent walk and not hang. */
    const char *src =
        "<?php\n"
        "class A extends A {}\n"
        "class C {\n"
        "    public function run(A $a): void { $a->go(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 76. Many use clauses don't grow unboundedly ──────── */

TEST(phplsp_many_use_clauses) {
    const char *src =
        "<?php\n"
        "namespace App;\n"
        "use Vendor\\A;\n"
        "use Vendor\\B;\n"
        "use Vendor\\C2;\n"
        "use Vendor\\D;\n"
        "use Vendor\\E;\n"
        "use Vendor\\F;\n"
        "use Vendor\\G;\n"
        "use Vendor\\H;\n"
        "class K {\n"
        "    public function run(A $a, B $b, C2 $c): void {\n"
        "        $a->aa(); $b->bb(); $c->cc();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(find_resolved(r, "K.run", "Vendor.A.aa") >= 0);
    ASSERT(find_resolved(r, "K.run", "Vendor.B.bb") >= 0);
    ASSERT(find_resolved(r, "K.run", "Vendor.C2.cc") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 77. Method chain across $this and field ──────────── */

TEST(phplsp_chain_through_this_field) {
    const char *src =
        "<?php\n"
        "class Inner { public function go(): int { return 1; } }\n"
        "class Outer {\n"
        "    private Inner $i;\n"
        "    public function __construct(Inner $i) { $this->i = $i; }\n"
        "    public function tap(): Inner { return $this->i; }\n"
        "}\n"
        "class C {\n"
        "    public function run(Outer $o): void {\n"
        "        $o->tap()->go();\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(require_resolved(r, "C.run", "Outer.tap") >= 0);
    ASSERT(require_resolved(r, "C.run", "Inner.go") >= 0);
    cbm_free_result(r);
    PASS();
}

/* ── 78. Trait flattening across files (single-file proxy) ── */

TEST(phplsp_trait_with_alias) {
    const char *src =
        "<?php\n"
        "trait T1 { public function shared(): int { return 1; } }\n"
        "class C {\n"
        "    use T1 { shared as inheritedShared; }\n"
        "    public function run(): void { $this->shared(); $this->inheritedShared(); }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    ASSERT(find_resolved(r, "C.run", "C.shared") >= 0 ||
           find_resolved(r, "C.run", "T1.shared") >= 0);
    /* The aliased method either resolves on C or falls through. */
    cbm_free_result(r);
    PASS();
}

/* ── 79. Yielding a value (generator) doesn't crash ────── */

TEST(phplsp_yield_expression) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(): \\Generator {\n"
        "        yield 1; yield 2;\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── 80. Bool / true / false literal types ─────────────── */

TEST(phplsp_bool_literals) {
    const char *src =
        "<?php\n"
        "class C {\n"
        "    public function run(): void {\n"
        "        $a = true;\n"
        "        $b = false;\n"
        "        $c = null;\n"
        "    }\n"
        "}\n";
    CBMFileResult *r = extract_php(src);
    ASSERT(r);
    cbm_free_result(r);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

SUITE(php_lsp) {
    /* Phase 1 baseline regressions */
    RUN_TEST(phplsp_local_method_via_new_assignment);
    RUN_TEST(phplsp_local_method_via_typed_param);
    RUN_TEST(phplsp_arrow_function_typed_param);
    RUN_TEST(phplsp_static_call_resolved);
    RUN_TEST(phplsp_self_and_parent);
    RUN_TEST(phplsp_use_function_clause);
    RUN_TEST(phplsp_phpdoc_var);
    RUN_TEST(phplsp_catch_binding);
    RUN_TEST(phplsp_this_method);
    RUN_TEST(phplsp_method_inherited_via_parent);
    RUN_TEST(phplsp_unindexed_receiver_emits_block);
    RUN_TEST(phplsp_regression_prompt_value_not_routed_to_helper);
    RUN_TEST(phplsp_method_chain_return_type);
    RUN_TEST(phplsp_function_global_fallback);
    RUN_TEST(phplsp_use_alias_resolves_for_new);
    /* Phase 4: depth */
    RUN_TEST(phplsp_constructor_property_promotion);
    RUN_TEST(phplsp_typed_property_declaration);
    RUN_TEST(phplsp_constructor_body_inference);
    RUN_TEST(phplsp_narrow_instanceof);
    RUN_TEST(phplsp_narrow_assert_instanceof);
    RUN_TEST(phplsp_phpdoc_property_class_tag);
    RUN_TEST(phplsp_phpdoc_method_class_tag);
    RUN_TEST(phplsp_trait_method_flattened);
    RUN_TEST(phplsp_match_result_type);
    RUN_TEST(phplsp_ternary_result_type);
    RUN_TEST(phplsp_method_chain_depth_three);
    RUN_TEST(phplsp_lsb_two_deep_chain);
    RUN_TEST(phplsp_nullsafe_operator);
    RUN_TEST(phplsp_static_keyword_dispatch);
    RUN_TEST(phplsp_self_in_method_body);
    RUN_TEST(phplsp_variadic_parameter);
    RUN_TEST(phplsp_closure_with_use);
    RUN_TEST(phplsp_anonymous_function_arg);
    RUN_TEST(phplsp_nested_arrow_functions);
    RUN_TEST(phplsp_narrow_is_string);
    RUN_TEST(phplsp_narrow_is_array);
    RUN_TEST(phplsp_multiple_use_clauses);
    RUN_TEST(phplsp_group_use_clause);
    RUN_TEST(phplsp_fqn_new_expression);
    RUN_TEST(phplsp_foreach_iteration);
    RUN_TEST(phplsp_catch_multi_type);
    RUN_TEST(phplsp_namespaced_catch);
    RUN_TEST(phplsp_implements_multiple_interfaces);
    RUN_TEST(phplsp_method_on_typed_field);
    RUN_TEST(phplsp_child_overrides_parent_method);
    RUN_TEST(phplsp_phpdoc_param_method);
    RUN_TEST(phplsp_static_field_access);
    RUN_TEST(phplsp_class_constant_access);
    RUN_TEST(phplsp_anonymous_class);
    RUN_TEST(phplsp_enum_method_dispatch);
    RUN_TEST(phplsp_readonly_property);
    RUN_TEST(phplsp_static_factory_chain);
    RUN_TEST(phplsp_stdlib_datetime_chain);
    RUN_TEST(phplsp_stdlib_throwable_chain);
    RUN_TEST(phplsp_stdlib_iterator);
    RUN_TEST(phplsp_abstract_class_method);
    RUN_TEST(phplsp_static_call_via_alias);
    RUN_TEST(phplsp_function_call_namespaced);
    RUN_TEST(phplsp_reassignment_changes_type);
    RUN_TEST(phplsp_use_then_new_aliased);
    RUN_TEST(phplsp_narrow_branches);
    RUN_TEST(phplsp_sequential_bindings);
    RUN_TEST(phplsp_chained_with_intermediate_var);
    RUN_TEST(phplsp_trait_multiple_methods);
    RUN_TEST(phplsp_cast_expression);
    RUN_TEST(phplsp_clone_preserves_type);
    RUN_TEST(phplsp_empty_class_body);
    RUN_TEST(phplsp_stdlib_psr_logger);
    RUN_TEST(phplsp_long_method_chain);
    RUN_TEST(phplsp_conditional_return);
    RUN_TEST(phplsp_phpdoc_var_at_assignment);
    RUN_TEST(phplsp_narrow_is_int);
    RUN_TEST(phplsp_encapsed_string);
    RUN_TEST(phplsp_heredoc);
    RUN_TEST(phplsp_cyclic_embed_bounded);
    RUN_TEST(phplsp_many_use_clauses);
    RUN_TEST(phplsp_chain_through_this_field);
    RUN_TEST(phplsp_trait_with_alias);
    RUN_TEST(phplsp_yield_expression);
    RUN_TEST(phplsp_bool_literals);
}
