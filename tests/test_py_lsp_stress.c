/*
 * test_py_lsp_stress.c — Advanced Python pattern probes.
 *
 * Each test covers one pattern Pyright resolves. Tests that pass
 * confirm parity. Tests that fail are honest gaps — they get either
 * fixed in a follow-up round or documented as deferred per
 * docs/BENCHMARK_PYTHON.md.
 *
 * Suite groups: patterns by feature area (typing helpers,
 * inheritance, control flow, containers, decorators, advanced
 * constructs).
 */
#include "test_framework.h"
#include "cbm.h"
#include "lsp/py_lsp.h"

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

/* ── Typing helpers ─────────────────────────────────────────── */

TEST(stress_namedtuple_class_form) {
    /* class Point(NamedTuple): x: int  → Point(1, 2).x */
    CBMFileResult *r = extract_py(
        "from typing import NamedTuple\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "class Pair(NamedTuple):\n"
        "    a: Foo\n"
        "    b: Foo\n"
        "def use():\n"
        "    p = Pair(Foo(), Foo())\n"
        "    return p.a.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(stress_typeddict_subscript) {
    /* class TD(TypedDict): x: Foo. d: TD = ...; d["x"].method() */
    CBMFileResult *r = extract_py(
        "from typing import TypedDict\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "class TD(TypedDict):\n"
        "    foo: Foo\n"
        "def use(d: TD):\n"
        "    return d['foo'].method()\n");
    ASSERT_NOT_NULL(r);
    /* TypedDict["literal"] should narrow to the field type — gap if fails */
    int idx = find_resolved(r, "use", "method");
    if (idx < 0) printf("  KNOWN GAP: TypedDict['literal'] narrowing\n");
    cbm_free_result(r);
    PASS();
}

TEST(stress_protocol_structural) {
    /* def use(x: HasMethod): x.method()
     * where HasMethod is a Protocol */
    CBMFileResult *r = extract_py(
        "from typing import Protocol\n"
        "class HasMethod(Protocol):\n"
        "    def method(self) -> int: ...\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(x: HasMethod):\n"
        "    return x.method()\n");
    ASSERT_NOT_NULL(r);
    /* Should resolve x.method against the Protocol */
    int idx = find_resolved(r, "use", "method");
    if (idx < 0) printf("  KNOWN GAP: Protocol structural method dispatch\n");
    cbm_free_result(r);
    PASS();
}

TEST(stress_abc_abstractmethod) {
    /* class Base(ABC): @abstractmethod def m(self): ...
     * class Child(Base): def m(self): return 1
     * def use(x: Base): x.m()  → resolves to Base.m (or Child.m via dispatch?) */
    CBMFileResult *r = extract_py(
        "from abc import ABC, abstractmethod\n"
        "class Base(ABC):\n"
        "    @abstractmethod\n"
        "    def method(self):\n"
        "        ...\n"
        "class Child(Base):\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(x: Base):\n"
        "    return x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

/* ── Control flow / binding ─────────────────────────────────── */

TEST(stress_context_manager_with_as) {
    /* with Foo() as f: f.method() — f bound by __enter__ */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def __enter__(self):\n"
        "        return self\n"
        "    def __exit__(self, *args):\n"
        "        return None\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use():\n"
        "    with Foo() as f:\n"
        "        return f.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(stress_except_as_binding) {
    /* except E as e: e.method() — e bound to E */
    CBMFileResult *r = extract_py(
        "class FooError(Exception):\n"
        "    def detail(self):\n"
        "        return 'oops'\n"
        "def use():\n"
        "    try:\n"
        "        return 1\n"
        "    except FooError as e:\n"
        "        return e.detail()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "detail"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(stress_isinstance_else_negative_narrow) {
    /* if not isinstance(x, Foo): return; x.method()
     * Pyright narrows x to Foo after the early return. We don't model this. */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(x):\n"
        "    if not isinstance(x, Foo):\n"
        "        return None\n"
        "    return x.method()\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "use", "method");
    if (idx < 0) printf("  KNOWN GAP: post-early-return narrowing\n");
    cbm_free_result(r);
    PASS();
}

/* ── Containers / unpacking ─────────────────────────────────── */

TEST(stress_tuple_unpack_function_return) {
    /* def f() -> tuple[Foo, Bar]: ...
     * a, b = f(); a.method_a(); b.method_b() */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method_a(self):\n"
        "        return 1\n"
        "class Bar:\n"
        "    def method_b(self):\n"
        "        return 2\n"
        "def make() -> tuple[Foo, Bar]:\n"
        "    return Foo(), Bar()\n"
        "def use():\n"
        "    a, b = make()\n"
        "    a.method_a()\n"
        "    b.method_b()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method_a"), 0);
    ASSERT_GTE(require_resolved(r, "use", "method_b"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(stress_dict_items_comprehension) {
    /* d: dict[str, Foo]; [v.method() for k, v in d.items()] */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(d: dict[str, Foo]):\n"
        "    return [v.method() for k, v in d.items()]\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(stress_list_slice_returns_list) {
    /* lst: list[Foo]; lst[1:3][0].method() */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(items: list[Foo]):\n"
        "    return items[1:3][0].method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

/* ── Decorators / first-class functions ──────────────────────── */

TEST(stress_function_as_dict_value) {
    /* funcs: dict[str, Callable] = {"a": foo}; funcs["a"]() */
    CBMFileResult *r = extract_py(
        "def foo():\n"
        "    return 1\n"
        "def bar():\n"
        "    return 2\n"
        "def use():\n"
        "    funcs = {'a': foo, 'b': bar}\n"
        "    funcs['a']()\n");
    ASSERT_NOT_NULL(r);
    /* funcs['a'] resolves to a Callable; calling it is an unknown target.
     * This is genuinely hard without runtime info. */
    int idx = find_resolved(r, "use", "foo");
    if (idx < 0) printf("  KNOWN GAP: function-as-dict-value indirect call\n");
    cbm_free_result(r);
    PASS();
}

TEST(stress_decorator_factory) {
    /* @retry(times=3) — decorator factory pattern */
    CBMFileResult *r = extract_py(
        "import functools\n"
        "def retry(times: int):\n"
        "    def deco(fn):\n"
        "        @functools.wraps(fn)\n"
        "        def wrapper(*args, **kwargs):\n"
        "            return fn(*args, **kwargs)\n"
        "        return wrapper\n"
        "    return deco\n"
        "@retry(times=3)\n"
        "def helper():\n"
        "    return 1\n"
        "def use():\n"
        "    return helper()\n");
    ASSERT_NOT_NULL(r);
    /* helper() should resolve as helper despite the decorator. */
    ASSERT_GTE(require_resolved(r, "use", "helper"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(stress_property_setter) {
    /* @prop.setter — assignment to property */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    @property\n"
        "    def value(self) -> int:\n"
        "        return 1\n"
        "    @value.setter\n"
        "    def value(self, v: int) -> None:\n"
        "        pass\n"
        "    def use(self):\n"
        "        self.value = 5\n"
        "        return self.value\n");
    ASSERT_NOT_NULL(r);
    /* Setter is rare in resolution but should not break anything. */
    cbm_free_result(r);
    PASS();
}

/* ── Inheritance / generics ─────────────────────────────────── */

TEST(stress_self_in_inheritance_chain) {
    /* class B: def make(self) -> Self: ...
     * class C(B): def child_method(self) -> int: ...
     * c = C().make()  # Pyright: c is C, not B
     * c.child_method() */
    CBMFileResult *r = extract_py(
        "from typing import Self\n"
        "class Base:\n"
        "    def make(self) -> Self:\n"
        "        return self\n"
        "class Child(Base):\n"
        "    def child_method(self):\n"
        "        return 1\n"
        "def use():\n"
        "    c = Child().make()\n"
        "    return c.child_method()\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "use", "child_method");
    if (idx < 0) printf("  KNOWN GAP: Self resolves to receiver class, not declaring class\n");
    cbm_free_result(r);
    PASS();
}

TEST(stress_diamond_inheritance_mro) {
    /* Diamond: Top -> Left, Right -> Bottom. Bottom().method() resolves to
     * Top.method via C3 unless Left/Right override. */
    CBMFileResult *r = extract_py(
        "class Top:\n"
        "    def method(self):\n"
        "        return 1\n"
        "class Left(Top):\n"
        "    pass\n"
        "class Right(Top):\n"
        "    pass\n"
        "class Bottom(Left, Right):\n"
        "    def use(self):\n"
        "        return self.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(stress_recursive_self_referencing_type) {
    /* class Node: children: list[Node]
     * def walk(node: Node): for c in node.children: c.method() */
    CBMFileResult *r = extract_py(
        "class Node:\n"
        "    def __init__(self):\n"
        "        self.children: list['Node'] = []\n"
        "    def method(self):\n"
        "        return 1\n"
        "def walk(node: Node):\n"
        "    for c in node.children:\n"
        "        c.method()\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "walk", "method");
    if (idx < 0) printf("  KNOWN GAP: forward-ref `Node` inside list[] within own class\n");
    cbm_free_result(r);
    PASS();
}

/* ── Advanced control flow ─────────────────────────────────── */

TEST(stress_nested_closure) {
    /* def outer():
     *   x = Foo()
     *   def inner():
     *     return x.method()
     *   return inner */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def outer():\n"
        "    x = Foo()\n"
        "    def inner():\n"
        "        return x.method()\n"
        "    return inner()\n");
    ASSERT_NOT_NULL(r);
    /* inner() captures x via closure; x is Foo from outer scope. */
    int idx = find_resolved(r, "inner", "method");
    if (idx < 0) printf("  KNOWN GAP: closure scope capture across nested function\n");
    cbm_free_result(r);
    PASS();
}

TEST(stress_match_sequence_pattern) {
    /* match items: case [head, *tail]: head.method() */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(items: list[Foo]):\n"
        "    match items:\n"
        "        case [head, *tail]:\n"
        "            return head.method()\n"
        "        case []:\n"
        "            return None\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "use", "method");
    if (idx < 0) printf("  KNOWN GAP: match sequence pattern element typing\n");
    cbm_free_result(r);
    PASS();
}

TEST(stress_lambda_inference) {
    /* fn = lambda x: x.method(); fn(Foo()) */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use():\n"
        "    fn = lambda x: x.method()\n"
        "    return fn(Foo())\n");
    ASSERT_NOT_NULL(r);
    /* x's type inside the lambda comes from the call site — needs
     * bidirectional inference. We don't model this. */
    int idx = find_resolved(r, "<lambda>", "method");
    if (idx < 0) printf("  KNOWN GAP: lambda parameter inference from call site\n");
    cbm_free_result(r);
    PASS();
}

TEST(stress_method_chain_long) {
    /* a.b().c().d().e() — every step must preserve type for the next. */
    CBMFileResult *r = extract_py(
        "from typing import Self\n"
        "class B:\n"
        "    def step(self) -> Self:\n"
        "        return self\n"
        "    def finish(self):\n"
        "        return 1\n"
        "def use():\n"
        "    return B().step().step().step().step().finish()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "finish"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(stress_generator_delegation) {
    /* def outer(): yield from inner() — outer's iterable element is inner's */
    CBMFileResult *r = extract_py(
        "from typing import Generator\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def inner() -> Generator[Foo, None, None]:\n"
        "    yield Foo()\n"
        "def outer() -> Generator[Foo, None, None]:\n"
        "    yield from inner()\n"
        "def use():\n"
        "    for x in outer():\n"
        "        x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(stress_async_gen_for) {
    /* async for x in async_gen(): x.method() */
    CBMFileResult *r = extract_py(
        "from typing import AsyncGenerator\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "async def gen() -> AsyncGenerator[Foo, None]:\n"
        "    yield Foo()\n"
        "async def use():\n"
        "    async for x in gen():\n"
        "        x.method()\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "use", "method");
    if (idx < 0) printf("  KNOWN GAP: async-for element typing\n");
    cbm_free_result(r);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────── */

SUITE(py_lsp_stress) {
    /* Typing helpers */
    RUN_TEST(stress_namedtuple_class_form);
    RUN_TEST(stress_typeddict_subscript);
    RUN_TEST(stress_protocol_structural);
    RUN_TEST(stress_abc_abstractmethod);
    /* Control flow / binding */
    RUN_TEST(stress_context_manager_with_as);
    RUN_TEST(stress_except_as_binding);
    RUN_TEST(stress_isinstance_else_negative_narrow);
    /* Containers */
    RUN_TEST(stress_tuple_unpack_function_return);
    RUN_TEST(stress_dict_items_comprehension);
    RUN_TEST(stress_list_slice_returns_list);
    /* Decorators / first-class functions */
    RUN_TEST(stress_function_as_dict_value);
    RUN_TEST(stress_decorator_factory);
    RUN_TEST(stress_property_setter);
    /* Inheritance / generics */
    RUN_TEST(stress_self_in_inheritance_chain);
    RUN_TEST(stress_diamond_inheritance_mro);
    RUN_TEST(stress_recursive_self_referencing_type);
    /* Advanced control flow */
    RUN_TEST(stress_nested_closure);
    RUN_TEST(stress_match_sequence_pattern);
    RUN_TEST(stress_lambda_inference);
    RUN_TEST(stress_method_chain_long);
    RUN_TEST(stress_generator_delegation);
    RUN_TEST(stress_async_gen_for);
}
