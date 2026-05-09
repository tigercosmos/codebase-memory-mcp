# Python LSP integration plan

Mirrors the existing `internal/cbm/lsp/{go_lsp,c_lsp}.{c,h}` pattern: vendor a reference LSP server's source, port the type-resolution algorithms into a self-contained C module driven by tree-sitter, and grow the suite test-first.

## 1. Reference vendoring

Cloned shallow into `/tmp/python-lsp-references/` (read-only, not committed). **Per challenger, pin to specific commit SHAs before mining — `main depth=1` shifts under you within days. Document the SHAs in this file and in the generated stdlib data header.**

| Source | Pin (record SHA before mining) | Size | Role |
|---|---|---|---|
| [microsoft/pyright](https://github.com/microsoft/pyright) | commit `53969cf1e39498f097437b4ee92cc2fad00cf641` (2026-05-08) | 52 MB | Primary algorithmic reference. TypeScript, MIT. Releases ~weekly. Mining `main` is unstable. |
| [python/typeshed](https://github.com/python/typeshed) | commit `a7912d521e16ff63caf7a8b64b9072542be36777` (2026-05-08) | 33 MB (stdlib 5.1 MB, 739 `.pyi` files) | Stdlib stub source for `gen-py-stdlib.py`. |
| [davidhalter/jedi](https://github.com/davidhalter/jedi) | commit `3102215478fe07b965dcd8221c17436d1dd7e8ac` (2026-05-08, master). For dev-time oracle pin to PyPI `jedi==0.19.2` — **do NOT use 0.20.0** (dropped 3.9 support). | 3.2 MB | Secondary reference + dev-time oracle (subprocess `jedi.Script(src).goto(line, col)` for test-corpus validation). |
| [typeshed-client](https://pypi.org/project/typeshed-client/) | `2.8.2` (PyPI) | — | Parser layer for `gen-py-stdlib.py`. **Use this instead of raw `ast`** — handles version-guard expansion, overload stacks, ParamSpec/TypeVarTuple noise filtering. Versioned and maintained. |
| [DetachHead/basedpyright](https://github.com/DetachHead/basedpyright) | (consider as v2 alternative) | — | Pyright fork on PyPI without Node.js, biweekly rebase, more granular UNKNOWN-vs-Any classification. Same algorithmic surface as upstream Pyright. |
| [facebook/pyrefly](https://github.com/facebook/pyrefly) | (re-evaluate after v1) | — | Rust, MIT, open-sourced May 2025. Closer in spirit to a C reimplementation. Currently active alpha — re-assess as reference for v2. |

Key Pyright files we will mine (sizes confirmed from clone):

| File | LOC | Maps to |
|---|---:|---|
| `analyzer/binder.ts` | 4,723 | scope construction + symbol-table walk |
| `analyzer/typeEvaluator.ts` | 28,955 | expression typing (`getTypeOfExpression`, `getTypeOfMemberAccess`, `getTypeOfCall`) |
| `analyzer/typeUtils.ts` | 4,511 | type ops: union normalize, narrow, substitute |
| `analyzer/types.ts` | 3,999 | `TypeCategory` enum (`Unbound`, `Unknown`, `Any`, `Never`, `Function`, `Overloaded`, `Class`, `Module`, `Union`, `TypeVar`) + flags |
| `analyzer/importResolver.ts` | 2,539 | package + relative + namespace import resolution |
| `analyzer/parseTreeUtils.ts` | 2,747 | AST helpers (most tree-sitter equivalents already exist) |
| `analyzer/declaration.ts` | 300 | declaration kinds (Intrinsic, Variable, Param, Class, Function, …) |
| `analyzer/scope.ts` | 267 | `ScopeType` enum (`TypeParameter`, `Comprehension`, `Function`, `Class`, `Module`, `Builtin`) |
| `analyzer/decorators.ts` | 606 | `@dataclass`, `@property`, `@classmethod`, … |
| `analyzer/dataClasses.ts` | 1,597 | dataclass `__init__` synthesis |
| `analyzer/protocols.ts` | 880 | structural protocol matching |
| `analyzer/patternMatching.ts` | 2,253 | PEP 634 `match`/`case` capture binding |
| `analyzer/enums.ts` | 750 | `Enum` value typing |
| `analyzer/namedTuples.ts` | 513 | `NamedTuple` synthesis |
| `analyzer/checker.ts` | 7,633 | not ported — diagnostics only, irrelevant for call resolution |
| `parser/*.ts` | — | not ported — we use tree-sitter Python (already vendored at `internal/cbm/vendored/grammars/python/`) |

We will not ship the references; we mine them during development.

## 2. Existing architecture (fixed reference points)

| Component | Path | Role |
|---|---|---|
| Type representation | `internal/cbm/lsp/type_rep.{c,h}` | `CBMType` tagged union (NAMED, POINTER, SLICE, MAP, CHANNEL, FUNC, INTERFACE, STRUCT, BUILTIN, TUPLE, TYPE_PARAM, REFERENCE, RVALUE_REF, TEMPLATE, ALIAS) |
| Type/function registry | `internal/cbm/lsp/type_registry.{c,h}` | `CBMRegisteredFunc`, `CBMRegisteredType`; lookup by qn / receiver / args / types; alias chain following |
| Lexical scope | `internal/cbm/lsp/scope.{c,h}` | `CBMScope` chain with `CBM_SCOPE_MAX_BINDINGS=64` per frame |
| Go LSP entry | `internal/cbm/lsp/go_lsp.{c,h}` | `cbm_run_go_lsp` (single), `_cross`, `_batch_cross`; `cbm_go_stdlib_register` |
| C/C++ LSP entry | `internal/cbm/lsp/c_lsp.{c,h}` | `cbm_run_c_lsp` (single), `_cross`, `_batch_cross`; `cbm_c_stdlib_register`, `cbm_cpp_stdlib_register` |
| Generated stdlib data | `internal/cbm/lsp/generated/{c,cpp,go}_stdlib_data.c` | Auto-generated; go_stdlib is 30,630 lines (2,328 fns, 321 types, 34 packages) |
| CGo umbrella | `internal/cbm/lsp_all.c` | `#include`s every lsp/*.c (CGo only compiles top-level package files) |
| Single-file wiring | `internal/cbm/cbm.c::cbm_extract_file` (lines 354–360, 409–410) | Calls `cbm_run_go_lsp` / `cbm_run_c_lsp` after unified extraction |
| Output sink | `CBMFileResult.resolved_calls` (`internal/cbm/cbm.h:416`) | LSP-resolved calls with qn / strategy / confidence — populated but not yet read by `pass_calls.c` |
| Test suites | `tests/test_go_lsp.c` (1,189 LOC, 49 tests), `tests/test_c_lsp.c` (15,816 LOC) | TDD pattern: `extract_X(source)` helper, `find_resolved`/`require_resolved` assertions, per-category sections, single `SUITE(go_lsp)` / `SUITE(c_lsp)` |
| Make wiring | `Makefile.cbm:308–310, 329` | `TEST_GO_LSP_SRCS`, `TEST_C_LSP_SRCS` added to `ALL_TEST_SRCS` |
| Test runner | `tests/test_main.c:42–43, 115–116` | `extern void suite_go_lsp(void);` + `RUN_SUITE(go_lsp)` |
| Python today | `internal/cbm/grammar_python.c` (3 lines, just bridges tree-sitter), `lang_specs.c:191–203` (node-type lists), `extract_imports.c:216–233` (`parse_python_imports`), `extract_defs.c:669, 707, 2558, 2989` (Python def metadata) | Generic extraction works; **no `py_lsp` module, no Python stdlib data, no type-aware call resolution** |

## 3. Type-rep extensions for Python

Python adds shapes Go and C++ don't have. Add to `type_rep.{c,h}`:

| New kind | Pyright analog | Why we need it |
|---|---|---|
| `CBM_TYPE_UNION` | `TypeCategory.Union` | `A | B`, `Optional[T]`, `Union[A, B]`. Needed for typeshed signatures — most stdlib functions return unions. Stored as sorted-canonical list with structural equality. |
| `CBM_TYPE_LITERAL` | (LiteralValue inside Class) | `Literal["foo", 3]`. Used heavily by typeshed and by overload selection. |
| `CBM_TYPE_PROTOCOL` | (ClassType + protocol flag) | `typing.Protocol`. Structural, not nominal. Largely overlaps with INTERFACE but matched structurally. |
| `CBM_TYPE_MODULE` | `TypeCategory.Module` | `import os` then `os.path.join` — module-attribute lookup distinct from class-attribute lookup. |
| `CBM_TYPE_CALLABLE` | (Function with no signature) | `Callable[[A, B], R]` and untyped callables; FUNC requires named params we may not have. |

Reuse existing kinds: `BUILTIN` for `int`/`str`/`bytes`/`bool`/`float`/`complex`/`None`/`bytearray`; `TUPLE` for `tuple[A, B]`; `TEMPLATE` for `list[T]`/`dict[K, V]`/`set[T]`; `TYPE_PARAM` for TypeVars; `ALIAS` for `type X = Y` and `X: TypeAlias = Y`; PEP 695 `class Foo[T]:` syntax populates the existing `type_param_names` field on `CBMRegisteredType`.

Tests: `test_pylsp_type_rep` — round-trip construction, union normalization (`A | A → A`, `A | None` flattening), literal equality, protocol method-set matching, alias resolution with cycle guard.

## 4. New files

```
internal/cbm/lsp/py_lsp.h                      # ~150 LOC, header
internal/cbm/lsp/py_lsp.c                      # ~3,000–5,000 LOC eventually
internal/cbm/lsp/generated/python_stdlib_data.c # auto-generated, ~10–20K LOC
scripts/gen-py-stdlib.py                        # generator using `ast` module on typeshed/stdlib
tests/test_py_lsp.c                             # ~1,500–3,000 LOC, paralleling test_go_lsp.c
```

Edits:

```
internal/cbm/lsp_all.c             # +#include "lsp/py_lsp.c" and generated/python_stdlib_data.c
internal/cbm/cbm.c                 # +if (language == CBM_LANG_PYTHON) cbm_run_py_lsp(...);
internal/cbm/lsp/type_rep.{c,h}    # add UNION, LITERAL, PROTOCOL, MODULE, CALLABLE kinds
Makefile.cbm                       # +TEST_PY_LSP_SRCS = tests/test_py_lsp.c, add to ALL_TEST_SRCS
tests/test_main.c                  # +extern void suite_py_lsp(void); +RUN_SUITE(py_lsp);
THIRD_PARTY.md                     # add Pyright + typeshed + Jedi attribution (referenced during dev only)
```

## 5. Phased TDD slices

Each phase is its own PR-sized increment. No phase merges with red tests.

### Phase 0 — pre-flight fixes (NEW, per challenger; locked decisions)

Before any py_lsp code is written, fix two issues in shared infrastructure that are silent bugs for Python:

- **Convert `CBMScope` to dynamic linked-frame allocation** (per §7 decision). Replace `bindings[CBM_SCOPE_MAX_BINDINGS]` fixed array with chunked linked list:
  ```c
  typedef struct CBMScopeChunk {
      CBMVarBinding bindings[16];
      int used;
      struct CBMScopeChunk* next;
  } CBMScopeChunk;
  typedef struct CBMScope {
      struct CBMScope* parent;
      CBMScopeChunk* chunks;   // first chunk; new chunks prepended
  } CBMScope;
  ```
  `cbm_scope_bind` appends to current chunk, allocates new chunk when `used == 16`. `cbm_scope_lookup` walks chunks then parent. Existing API preserved. **Cross-language regression run required**: `test_go_lsp` + `test_c_lsp` must stay green with no test changes.
- **Add a recursion-depth cap (depth=16) to MRO/method-lookup walks** mirroring the existing depth-16 cap on `cbm_type_resolve_alias`. Diamond hierarchies in real Python (SQLAlchemy, Django, Zope) exceed 20 levels — without a cap a pathological hierarchy stack-overflows the C process rather than degrading to UNKNOWN. The cap is shared infra and applies to Go (struct embedding chain) and C++ (CRTP / multi-inheritance) too.

Tests: `scope_dynamic_growth_300_bindings` (allocates 300 bindings, asserts all retrievable), `mro_depth_cap_returns_unknown_at_17` (constructs 20-deep diamond hierarchy, asserts UNKNOWN at depth 17 not crash). Cross-language: add `golsp_scope_300_bindings`, `clsp_scope_300_bindings` to verify Go/C++ LSP unaffected by the scope refactor.

### Phase 1 — type-rep extensions
Add `UNION`, `LITERAL`, `PROTOCOL`, `MODULE`, `CALLABLE` constructors and operations.
Tests: `pylsp_type_union_normalize`, `pylsp_type_literal_equality`, `pylsp_type_optional_is_union_none`, `pylsp_type_protocol_method_set`, `pylsp_type_module_attribute_lookup`, `pylsp_type_callable_arity`.

### Phase 2 — scaffold + entry point + smoke
Create `py_lsp.{c,h}`, `PyLSPContext` mirroring `GoLSPContext`. Implement `cbm_run_py_lsp` that builds an empty registry, runs the binder, returns. Wire `cbm_extract_file` to call it. Add `lsp_all.c` include. Add `tests/test_py_lsp.c` skeleton with helper `extract_py(source)`. Wire test runner.
Tests: `pylsp_smoke_empty`, `pylsp_smoke_one_function`, `pylsp_smoke_one_class`, `pylsp_no_crash_on_syntax_error`.

### Phase 3 — imports
Port `importResolver.ts` algorithm subset (we only need symbol → module-qn, not on-disk resolution since the pipeline already gives us the import map). Handle `import X`, `import X.Y as Z`, `from X import a, b`, `from X import a as b`, `from . import x`, `from ..pkg import x`, `from X import *` (best-effort, treat as wildcard re-export). Honor `if TYPE_CHECKING:` (type-only imports still bind names).
Tests: `pylsp_import_simple`, `_import_aliased`, `_import_from`, `_import_from_aliased`, `_import_relative_one_dot`, `_import_relative_two_dots`, `_import_star_best_effort`, `_import_typing_only`.

### Phase 4 — scope + binding
Port `binder.ts` visit overrides for Python (32 in Pyright). Bind: assignments, annotated assignments, augmented assignments, walrus, `for`, `with as`, `except as`, function params (positional, keyword-only, `*args`, `**kwargs`, defaults, annotations), `global`, `nonlocal`, comprehension scopes, lambda, PEP 634 `match`/`case` captures, PEP 695 type-parameter scopes, `del`.
Tests: `pylsp_assign_simple`, `_annotated_assign`, `_multi_assign_join`, `_walrus`, `_for_loop_var`, `_with_as`, `_except_as`, `_param_default`, `_param_starargs`, `_global_nonlocal`, `_match_capture`, `_pep695_type_param`, `_comprehension_scope`, `_lambda_scope`.

### Phase 5 — expression typing (no method dispatch yet)
Port the typeEvaluator subset. Literals (int/str/bytes/bool/None → BUILTIN or LITERAL); names (scope → registry → UNKNOWN); BinOp via dunders (`__add__`, `__radd__`, …) starting with int/str/list arithmetic; UnaryOp; Compare → bool; Tuple/List/Dict/Set literal → TEMPLATE container with element-type inference; Subscript with `__getitem__` plus typed shortcuts for `list[T][int]`, `dict[K, V][K]`, `tuple[A, B][0]`; Lambda → CALLABLE; `a if c else b` → union(a, b); comprehensions (list/set/dict/gen) → TEMPLATE container; star/double-star unpack tracked for arg-count purposes.
Tests: `pylsp_literal_int_str`, `_binop_arithmetic`, `_binop_string_concat`, `_compare`, `_list_literal`, `_dict_literal`, `_subscript_list_int`, `_subscript_dict_key`, `_subscript_tuple_int`, `_lambda_inferred`, `_conditional_expr_union`, `_listcomp`, `_dictcomp`, `_setcomp`, `_gencomp`.

### Phase 5.5 — spike `getTypeOfMemberAccess` port (NEW, per challenger)
**Time-box: 1 day.** Before committing to Phase 6's full scope, spike a minimal `obj.method()` resolver covering: type-of-obj resolution, single-level MRO walk, basic property/classmethod/staticmethod handling. Measure C-LOC cost. **If the spike exceeds ~1,200 LOC at acceptable quality, the full plan is undersized — pause and renegotiate scope before continuing.** This is the load-bearing test of the "port ~3,000 C lines of relevant typeEvaluator subset" estimate.

### Phase 6 — attribute resolution + method dispatch (the core)
For `obj.attr`: resolve type of `obj`; if class, walk MRO via C3 linearization for `attr` (with explicit depth-cap from Phase 0); if module, look up exported symbol; if Protocol, structural match; honor `@property` (return getter's return), `@classmethod`, `@staticmethod`, instance methods; partial descriptor protocol (`__get__`); cleanly bail (UNKNOWN) on `__getattr__` / `__getattribute__` rather than false-resolve. `self.x = …` inside `__init__` adds field to enclosing class.
Tests: `pylsp_method_simple`, `_method_inherited`, `_method_diamond_mro`, `_method_protocol`, `_classmethod`, `_staticmethod`, `_property_getter`, `_module_attribute`, `_method_chain`, `_dunder_init_self_attr`, `_method_super_call`.

### Phase 7 — decorators
Decorators rewrite the type of decorated symbols. `@dataclass` synthesizes `__init__` from annotated class fields (port `dataClasses.ts`). `@property` / `@x.setter` / `@x.deleter` change the symbol shape. `@classmethod`, `@staticmethod`, `@abstractmethod`, `@functools.wraps` (identity), `@functools.cache`/`@lru_cache` (identity for our purposes), `@overload` recorded so calls match the right signature, user decorators returning a callable (substitute return type).
Tests: `_decorator_identity`, `_decorator_property`, `_decorator_dataclass_init`, `_decorator_dataclass_field`, `_decorator_classmethod`, `_decorator_staticmethod`, `_decorator_overload`, `_decorator_user_returning_callable`, `_decorator_functools_wraps`.

### Phase 8 — class hierarchy + generics
C3 linearization for multi-inheritance MRO; `super()` resolution to next-in-MRO method; `Generic[T]` and PEP 695 syntax; type-parameter substitution at instantiation (`Foo[int]() → bind T=int`); `abc.ABC` / `@abstractmethod` (resolution-equivalent to concrete; just a flag); `Enum` value typing (port `enums.ts`); `NamedTuple` synthesis (port `namedTuples.ts`).
Tests: `_class_simple`, `_inheritance_single`, `_inheritance_diamond`, `_generic_class_instantiation`, `_super_call`, `_pep695_generic_class`, `_pep695_type_alias`, `_abc_method_resolution`, `_enum_value_type`, `_namedtuple_field`.

### Phase 9 — cross-file + batch
`cbm_run_py_lsp_cross` accepts caller-supplied `CBMLSPDef[]` summarizing every cross-file class/method/function/module member with its module QN, return-types-as-text, base classes, method names. Re-parse and resolve. `cbm_batch_py_lsp_cross` for parallel multi-file. Special concern for Python: `from foo import bar` semantics — `bar` may be class, function, module, or re-export; the cross-file def list must encode kind. Re-exports via `__all__` and bare `from .x import y` need handling.
Tests: `_crossfile_method_dispatch`, `_crossfile_inheritance`, `_crossfile_protocol`, `_crossfile_module_attribute`, `_crossfile_dataclass_field`, `_crossfile_re_export`, `_crossfile_init_re_export` (`pkg/__init__.py` reexports).

### Phase 9.5 — pipeline edge wiring (MANDATORY, moved earlier per challenger)
**Per challenger feedback, this is no longer optional and is moved to immediately after cross-file Python is working.** The Go and C++ LSPs already populate `result->resolved_calls` but the pipeline's `pass_calls.c` doesn't currently read it — calls still resolve via the textual-name registry. Shipping a third LSP into a dead buffer is not a meaningful milestone, and the longer wiring is deferred the larger the eventual coordination cost across three language implementations.

Update `src/pipeline/pass_calls.c::resolve_single_call` to:
1. Check `result->resolved_calls[]` first for a matching `(caller_qn, call_site)`.
2. If found and `confidence ≥ floor` (TBD — likely 0.6), emit edge with the LSP-resolved callee QN and `strategy="lsp_*"` from the entry.
3. Otherwise fall back to existing registry-based resolution.
Add a confidence floor knob (env var or config). Below the floor, leave it to the registry resolver (or emit nothing if both fail).
This is a cross-language change benefiting Go, C/C++, and Python simultaneously.

Tests: extend `tests/test_pipeline.c` with `pipeline_uses_lsp_resolved_calls`, `pipeline_falls_back_to_registry`, `pipeline_confidence_floor_filters`.

### Phase 10 — stdlib generator + data
Write `scripts/gen-py-stdlib.py` (Python 3.9 per global preference for the *generator runtime*; generated stubs target Python 3.12). **Use `typeshed-client` PyPI package as the parsing layer** rather than raw `ast` — it handles version-guard expansion (`if sys.version_info >= (3, 10):`), overload stack collapsing, and ParamSpec/TypeVarTuple noise filtering, all of which would otherwise be per-construct special-case code in our generator. Emit `cbm_python_stdlib_register` to `internal/cbm/lsp/generated/python_stdlib_data.c` mirroring the `go_stdlib_data.c` style (static method-name arrays, bulk `cbm_registry_add_type` / `cbm_registry_add_func` calls). **Header records the typeshed commit SHA mined.**

**Generator output validation:** add a CI step that compiles the generated `python_stdlib_data.c` and refuses to commit it if compilation fails. Catches when typeshed adds new stub idioms (e.g., PEP 742 `TypeIs`, PEP 696 TypeVar defaults) that the generator emits malformed C for.

Coverage target for v1 (top usage in real Python codebases): `builtins`, `typing`, `os`, `os.path`, `sys`, `collections` (esp. `defaultdict`, `OrderedDict`, `Counter`, `deque`), `collections.abc`, `functools`, `itertools`, `pathlib`, `json`, `re`, `dataclasses`, `enum`, `datetime`, `subprocess`, `logging`, `asyncio`, `unittest`, `argparse`, `contextlib`, `io`, `tempfile`, `shutil`, `inspect`, `abc`, `typing_extensions`, `warnings`, `weakref`, `copy`, `pickle`, `time`, `math`, `string`, `urllib.parse`, `urllib.request`, `http.client`, `socket`, `threading`, `multiprocessing`, `queue`. Skip `tkinter`, `turtle`, `curses`, `xml.*`, `email.*` for v1 — too big, low call volume in the projects we care about.
Tests: `pylsp_stdlib_os_path_join`, `_stdlib_collections_defaultdict`, `_stdlib_pathlib_path_method`, `_stdlib_json_loads_returns_any`, `_stdlib_logging_getlogger`, `_stdlib_re_compile_match`, `_stdlib_dataclasses_field`, `_stdlib_typing_optional_is_union`, `_stdlib_functools_partial_call`.

### Phase 11 — benchmarks + golden corpus
Run `scripts/benchmark-index.sh` against an existing Python repo (e.g. one of the indexed `~/project_dir/datadice/*` projects that's Python-heavy — TBD which). Track:
- **Absolute** per-file LSP overhead, not relative to baseline (per challenger). Budget: <10ms for a 500-line file. The "30% over baseline" framing is misleading because Python's current baseline is essentially zero LSP overhead.
- `resolved_calls.count / calls.count` ratio (target: ≥40% for normal application code, ≥70% with stdlib).
- Manual spot-check of 50 sampled resolutions for false-positive rate (target: < 5%).
- **Declared ceiling on accepted failure classes:** the benchmark measures against a documented list of cases where UNKNOWN is the *correct* answer, not a regression. Accepted classes: metaclass `__call__`, `__getattr__`/`__getattribute__`, `setattr` with non-literal name, `importlib.import_module(<dynamic_arg>)`, runtime monkey-patching, `eval`/`exec`. Anything else producing UNKNOWN is a measurable gap to investigate.
Compare to `test_go_lsp.c`'s coverage shape and tune.

## 6. Test-suite shape

Mirroring `tests/test_go_lsp.c` exactly:

```c
#include "test_framework.h"
#include "cbm.h"
#include "lsp/py_lsp.h"

static CBMFileResult *extract_py(const char *source) {
    return cbm_extract_file(source, (int)strlen(source), CBM_LANG_PYTHON,
                            "test", "main.py", 0, NULL, NULL);
}

static int find_resolved(const CBMFileResult *r, const char *callerSub, const char *calleeSub);
static int require_resolved(const CBMFileResult *r, const char *callerSub, const char *calleeSub);

/* ── Category N: ... ──────── */
TEST(pylsp_method_simple) { /* small Python source */ /* assert */ PASS(); }
...

SUITE(py_lsp) {
    RUN_TEST(pylsp_smoke_empty);
    RUN_TEST(pylsp_method_simple);
    ...
}
```

Estimated final test count by phase: Phase 1: 6, Phase 2: 4, Phase 3: 8, Phase 4: 14, Phase 5: 15, Phase 6: 11, Phase 7: 9, Phase 8: 10, Phase 9: 7, Phase 10: 9. **Total: ~93 tests** (Go LSP is at 49; Python's broader semantics warrant more coverage).

## 7. Decisions locked in (was: open questions)

User-confirmed decisions, 2026-05-08:

1. **Reference: Pyright primary + Jedi 0.19.2 as dev-time oracle.** Pyright's `binder.ts` / `typeEvaluator.ts` / `importResolver.ts` are the algorithmic reference (matching the gopls / clangd precedent). A Python subprocess runs `jedi.Script(src).goto(line, col)` over the test corpus to provide independent ground truth. The Jedi oracle lives in `tests/oracle/` (Python venv, jedi==0.19.2 pinned), is invoked from a test helper, is not a runtime dependency.
   - **Phase 2 addition:** add `tests/oracle/jedi_oracle.py` (subprocess JSON-RPC: take source + (line,col), return resolved QN). Add `tests/test_py_lsp.c` helper `run_jedi_oracle(src, line, col)` that shells out, parses JSON, returns expected QN. Tests in Phase 6 onward assert `py_lsp_resolution == jedi_oracle_resolution` for hand-curated cases.
   - **Generator runtime separate from oracle runtime:** stdlib generator uses Python 3.9 (global preference). Jedi oracle uses 3.10+ venv (since pinning to jedi==0.19.2 still requires a 3.9-compatible interpreter, but isolating the oracle in its own venv prevents cross-contamination). Document the venv setup in `tests/oracle/README.md`.

2. **Scope frame: dynamic linked-frame allocation.** Replaces the fixed 64-binding array with a growable arena-allocated linked list. Touches `internal/cbm/lsp/scope.{c,h}` which Go and C++ LSPs both depend on — therefore Phase 0 must include a regression run of `test_go_lsp` and `test_c_lsp` to confirm no behavior change at the existing-test scale. The new shape: `CBMScope { CBMScope* parent; CBMScopeChunk* chunks; }` where `CBMScopeChunk { CBMVarBinding bindings[CHUNK_SZ]; CBMScopeChunk* next; int used; }` with `CHUNK_SZ = 16`. `cbm_scope_bind` allocates a new chunk when `used == CHUNK_SZ`. Lookup walks chunks backward (most-recent-binding wins). Existing API surface preserved.
   - **Cross-language test:** explicit test in `test_go_lsp` and `test_c_lsp` for ≥100 bindings in one frame to verify no regression. Add `pylsp_scope_dynamic_growth_300_bindings` in py_lsp.

3. **Confidence floor: 0.6 (moderate, fixed default).** No env knob in v1 — keeps the surface smaller. We can add `CBM_LSP_CONFIDENCE_FLOOR` later if tuning shows it's needed. Below the floor, `pass_calls.c` falls back to the registry resolver and emits the registry's edge instead. Tests assert: floor=0.6 emits LSP edge when `lsp_param_type` (≥0.85) is present, falls back to registry when only an `lsp_best_guess` (~0.4) is present.

4. **Stdlib version target: union of Python 3.9–3.13 with per-symbol version guards.** Heaviest generator path, but matches the cross-version reality of indexed codebases. The generated `python_stdlib_data.c` records each func/type with `min_python_version` / `max_python_version` (encoded as packed `uint16_t` minor versions, e.g. `0x0309..0x030d`). `py_lsp` lookup at runtime needs to know the consumer file's target version — derive from project metadata if available, else default to 3.12 and only fall back to broader version guards on miss.
   - **Project-target detection:** read `pyproject.toml` `[tool.python] requires-python` or fall back to `python_requires` in `setup.py`. Default 3.12 if neither found. Cache per-project, not per-file.
   - **Generator complexity:** `typeshed-client` already exposes per-symbol version guards from `if sys.version_info >= (X, Y):` blocks — we wire those through directly, not re-derive.
   - Tests: `pylsp_stdlib_version_3_9_no_match`, `pylsp_stdlib_version_3_12_paramspec_resolves`, `pylsp_stdlib_version_3_13_typeis_resolves`.

5. **Vendoring policy** (not asked but recommended in §1): references stay external in `/tmp/python-lsp-references/` during dev; only the *generated outputs* (`internal/cbm/lsp/generated/python_stdlib_data.c`) ship in the repo. Matches the gopls/clangd precedent. Confirm or override later.

## 8. Out of scope (explicitly)

## 8. Out of scope (explicitly)

- Diagnostics emission (`checker.ts`). We only resolve calls; we don't report type errors.
- Hover/completion features. The LSP source is mined for type-resolution algorithms only, not for the LSP protocol surface.
- `__getattr__` / `__getattribute__` / metaclass `__call__` / `__class_getitem__` — bail to UNKNOWN.
- `eval`/`exec`-generated code, runtime monkey-patching, `setattr` with non-literal name.
- Cython, Stackless, MicroPython, and other dialects.
- `numpy.ndarray` / `pandas.DataFrame` rich shape inference — third-party stubs may be added later via the same generator.

## 9. Risks

- **typeEvaluator.ts is 28,955 lines.** Porting even 10% literally is unrealistic. Mitigation: port only the call-resolution-relevant subset (~3,000 C lines), borrowing decisions but not structure. Use the Go LSP's eager bottom-up shape rather than Pyright's lazy on-demand model.
- **Python's dynamism may cap resolution rate at ~70%.** Many calls are genuinely unresolvable statically (`getattr`, attribute set via dict, dynamic dispatch via `__call__`). Mitigation: aim for high precision on the resolvable subset, accept gaps cleanly (UNKNOWN, not garbage).
- **typeshed stubs use features (overload, ParamSpec, TypeVarTuple, Self, Concatenate) that complicate the generator.** Mitigation: in v1, the generator emits a single best signature per overload (the most-arg-permissive one) and elides ParamSpec/TypeVarTuple/Concatenate. Phase 10.5 follow-up handles them.
- **PEP 695 generic syntax is recent.** Mitigation: the tree-sitter Python grammar supports it; we'll exercise it with explicit tests in Phase 8.
- **Pyright is a moving target (Microsoft fork basedpyright also exists).** Mitigation: pin to a tag, document the commit hash we mined, treat algorithm changes as upstream we don't track.

## 10. Next concrete steps after plan approval

1. **Pin reference SHAs** (Pyright tag 1.1.407 / typeshed commit / Jedi 0.19.2) and record them in this file.
2. **Phase 0 PR:** `CBM_SCOPE_MAX_BINDINGS` bump 64→128 + MRO depth-cap helper + tests (~150 LOC). Cross-language change in shared infra; required before any py_lsp work.
3. **Phase 1 PR:** type-rep extensions + tests (~300 LOC change).
4. **Phase 2 PR:** `py_lsp.{c,h}` scaffold + entry-point wiring + smoke tests (~250 LOC).
5. **Phase 5.5 spike** (1-day time-box): minimal `getTypeOfMemberAccess` port. Decision gate: if >1,200 LOC, pause and renegotiate scope.
6. **Phase 9.5 PR:** `pass_calls.c` LSP-aware resolution + cross-language tests (~200 LOC).
7. Continue Phase 3..11 per phase. Each phase ≤ ~1,500 LOC, ≤ 15 tests, 1–3 day slice.

---

## Challenger Review (verbatim)

The following is the full output of the challenger gate run against the draft plan, per CLAUDE.md "Sharing the Report" requirement. Recommendations have already been incorporated into the phase plan above; this section preserves the original report for traceability.

### What Looks Good

The TDD-first, phase-per-PR structure with no merging on red tests is the right discipline for a feature this complex. Reusing `CBMLSPDef` for cross-file data avoids inventing a new serialization contract. The explicit out-of-scope list (diagnostics, hover, `__getattr__`, `eval`/exec) is precise and prevents scope creep. Deferring pipeline wiring to Phase 11/option-b is honest about user-visible impact rather than hiding it. The phase-10 stdlib coverage list is appropriately prioritized and the v1 skip list is defensible.

### Assumptions to Verify

**Assumption 1: Jedi is a "secondary reference" you might not even need.**

The plan treats Jedi as secondary to Pyright and frames the choice as "Pyright is the type-system reference, Jedi is more practical." That framing understates Jedi's relevance. Jedi's `goto()` and `infer()` APIs *directly answer the question* "what is the callee of this call expression?" — which is exactly what `cbm_run_py_lsp` needs to produce. Pyright answers a harder question: "is this call type-safe?" The 28,955-line `typeEvaluator.ts` is mostly type-checking machinery; the call-resolution subset is a smaller fraction. You're spending research effort on the harder problem when the simpler one (Jedi's inference core, ~3,600 lines) maps more directly to your output: a `(caller_qn, callee_qn, confidence)` triple.

*Consequence if wrong:* You end up porting type-checking plumbing (union narrowing, type guards, ParamSpec, overload resolution by type) that has no consumer in `pass_calls.c` and costs you 2-3x the implementation time.

**Assumption 2: The `CBM_SCOPE_MAX_BINDINGS = 64` limit per frame is adequate for Python.**

Go and C/C++ have modest local variable counts in typical functions. Python is different. A comprehension body is its own scope (PEP 709/compiler semantics), `match` arms each introduce bindings, dataclass `__init__` synthesis creates per-field bindings, and real-world Python code regularly has functions with 30+ named locals plus `*args` + `**kwargs` + annotated class attributes. The current scope frame holds exactly 64 bindings and **silently truncates** at `cbm_scope_bind`'s count check — there is no overflow to a second frame, no error, no diagnostic. At 64 the scope overflows silently and subsequent bindings are dropped. This is a precision-killer for large Python functions and a correctness bug for any file where the 65th local happens to be the receiver of the call you're trying to resolve.

*Consequence if wrong:* Silent false-negatives. Resolution rate will look fine in unit tests (which use small snippets) but fall off in real codebases. The Phase 12 benchmark may reveal this but by then 10 phases of work are in.

**Assumption 3: typeshed's `ast` module is a stable enough interface for the stdlib generator.**

`scripts/gen-py-stdlib.py` uses Python's `ast` module to parse `.pyi` files from typeshed. This works today, but typeshed stubs use features the `ast` module does not represent uniformly: `@overload` stacks, `ParamSpec`, `TypeVarTuple`, `Concatenate`, `Self`, `Unpack`, conditional version guards (`if sys.version_info >= (3, 10):`). The plan's mitigation ("emit a single best signature per overload, elide ParamSpec/TypeVarTuple/Concatenate") is correct as a v1 strategy, but it means the generator has *per-construct special-case logic* that must be updated each time typeshed adds new stub patterns. Typeshed is updated continuously and has no versioned releases — only commits. The plan says "pin to a tag" but typeshed has no release tags; only the `types-*` third-party stubs on PyPI are versioned.

*Consequence if wrong:* The generator breaks silently when typeshed adds new stub idioms (e.g., PEP 742 `TypeIs`, PEP 696 `TypeVar` defaults are already in typeshed). You discover this when re-running the generator on a fresh typeshed clone produces malformed C output.

**Assumption 4: Phase 11 deferral is safe.**

The plan defaults to option (b): defer pipeline wiring. The Go LSP has been populated into `resolved_calls` with zero consumer since it was first merged. So has C/C++. This work is shipping a third LSP that also writes to `resolved_calls` with zero consumer. Each deferred LSP increases the blast radius of the eventual wiring PR (the wiring code must handle all three language outputs simultaneously) and each LSP author drifts further from the context needed to wire it correctly.

*Consequence if wrong:* Phase 11 becomes a cross-language coordination task that never gets prioritized, and the Python LSP joins Go/C++ in permanent limbo.

**Assumption 5: 93 tests is adequate coverage for Python's surface area.**

The Go LSP has 49 tests for a language with a much smaller semantic surface. The plan estimates 93 for Python. But Python's dynamism, the number of scope edge cases (comprehension scopes, walrus in comprehensions, class scope non-closure, LEGB + nonlocal + global interactions, lambda in default argument), decorator combinations, and the `__init__`-self-attr inference make this a larger combinatorial space. For reference, Pyright's own test suite has thousands of individual cases just for the type narrowing rules. 93 tests covering 12 phases averages ~7.75 tests per phase; Phase 6 (attribute resolution + MRO) has 11 — which is thin for a feature covering C3 linearization, property descriptors, classmethod, staticmethod, `super()`, and module attribute lookup simultaneously.

### Alternatives Worth Considering

**Alternative 1: Subprocess Jedi as oracle, C reimplementation as fast path**

Rather than mining Pyright's TypeScript for algorithms to reimplement, consider a hybrid: run Jedi as a validation oracle during development (Python subprocess, `jedi.Script(source).goto(line, col)` for every call site in your test corpus) and build the C reimplementation to match Jedi's output rather than Pyright's theory. This is how `go_lsp` was effectively validated — against gopls behavior, not by porting gopls source. You get a ground truth for expected outputs in Phase 6-9 without needing to fully understand `typeEvaluator.ts`.

Trade-off: Jedi 0.20.0 (released May 2026) **dropped Python 3.9 support** and the maintainer has announced Zuban (Rust, AGPL-3.0) as Jedi's spiritual successor. Using Jedi as a dev-time oracle is still fine — you are not shipping it — but you cannot rely on it being importable in your test environment unless you pin to Jedi 0.19.2. Jedi's `infer()` also intentionally stops inference for very deep chains to avoid hangs, so it can produce UNKNOWN on the exact cases you most need to validate.

**Alternative 2: Swap Pyright reference for basedpyright**

basedpyright is a biweekly-rebased Pyright fork available on PyPI without Node.js. It ships stricter diagnostics and better annotation of exactly which type features are "unresolvable" vs "inferred as Any" — which maps better to the UNKNOWN vs RESOLVED distinction `py_lsp` needs to produce. It also runs without the Node.js runtime requirement. As a reference to mine, it has the same ~28K-LOC `typeEvaluator.ts`, so the porting surface is unchanged — but the diagnostics documentation in basedpyright is more granular about what the evaluator considers statically unresolvable.

Trade-off: It is still a TypeScript codebase of the same size. The benefit is operational (PyPI install, no Node) for dev-time oracle runs. As an algorithmic reference it is identical to upstream Pyright.

**Alternative 3: Pyrefly as reference (but not yet)**

Meta's Pyrefly (Rust, MIT, open-sourced May 2025) checks 1.8M LOC/second and achieves >70% conformance with the Python typing spec as of late 2025. Its Rust source would be much closer in spirit to a C reimplementation than TypeScript. It handles call resolution via the same approach (typeshed stubs + inference).

Trade-off: Pyrefly is explicitly labeled "active alpha" as of 2025, and the blog post says "not yet production-ready for complex codebases." Its conformance gaps are precisely in the complex features you need (ParamSpec, recursive generics, TypeVarTuple). It is not a viable reference today but should be re-evaluated after v1 of py_lsp ships.

**Alternative 4: Do not reimplement — shell out to Pyright/Jedi per batch**

The indexer could spawn a persistent Pyright LSP subprocess (`pyright-langserver --stdio`), send `textDocument/definition` for each call site via JSON-RPC, and use the response to populate `resolved_calls`. This avoids the 3,000–5,000 LOC reimplementation entirely.

Trade-off: This has been tried in the LSP-outside-editor context and has a known fatal flaw: Pyright's `textDocument/definition` responses are **incomplete unless every file in the workspace has been explicitly opened via `didOpen`**, as documented in pyright#10086. Batch resolution across a 5,000-file repo would require 5,000 `didOpen` messages before the first `definition` request returns correct results. The round-trip overhead at that scale makes this impractical for a synchronous indexer. Jedi has a `Project` API that avoids this problem, but again — the dev-time oracle pattern above is safer.

This alternative should be explicitly rejected in the plan with this rationale, not just left as an unaddressed option.

### Risk Register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| `CBM_SCOPE_MAX_BINDINGS=64` silently truncates Python functions with >64 locals | High | High | Raise to 128 or implement dynamic frame (linked list of arrays); add a test with exactly 65 locals |
| typeshed has no release tags; re-running generator on fresh clone produces different output | High | Med | Pin to a specific commit SHA, not `main`. Document SHA in generator header. |
| Phase 11 permanent deferral — all 3 LSPs stay wired to zero consumers | High | High | Make Phase 11 a hard gate, not optional. Schedule it in the same milestone as Phase 10 at the latest. |
| typeEvaluator.ts 3,000-line subset underestimate — actual MRO + overload + generic substitution alone exceeds this | Med | High | Time-box the port of `getTypeOfMemberAccess` in Phase 6 spike before committing to the full plan; fail fast if it takes >1,200 LOC |
| Jedi 0.20.0 drops Python 3.9 support; dev oracle environment breaks | High | Low | Pin to jedi==0.19.2 for oracle use; or use python3.10+ venv just for oracle |
| PEP 634 `match`/`case` binding creates >64 bindings in a single clause (union of all patterns) | Med | Med | Match-arm bodies should use child scopes, not the enclosing function scope. Verify scope push/pop in Phase 4 |
| False-positive resolution (wrong callee, high confidence) worse than false-negative (UNKNOWN) | Med | High | Precision > recall policy: prefer UNKNOWN over wrong QN. Add explicit confidence floor test. |
| gen-py-stdlib.py fails on new typeshed stub constructs (PEP 742, TypeIs, PEP 696 TypeVar defaults) | Med | Med | Add a validation pass: parse generator output through a C compiler after generation. Catch at CI. |
| Python `from X import *` wildcard imports require walking the entire exported symbol space | High | Low | Plan already flags "best-effort"; ensure the implementation emits MODULE type, not attempts expansion |
| Pyright 1.1.407 is at weekly release cadence; mined commit diverges from main within weeks | Low | Low | Document the exact commit hash mined. This is a research artifact, not a runtime dep. |

### Operational Concerns

**Error handling in the C pipeline — no explicit OOM/crash guard for py_lsp.**

Both `go_lsp` and `c_lsp` use arena allocation throughout and return silently on null returns from `cbm_arena_alloc`. This is the correct pattern for OOM: arena exhaustion causes null returns which propagate to `cbm_type_unknown()` returns, which cause UNKNOWN resolutions rather than crashes. The plan should explicitly confirm that `py_lsp` follows this same contract. The one failure mode that isn't covered: Python's deeply nested comprehensions (list-in-list-in-listcomp) can create very deep scope chains. The scope chain is a linked list of arena-allocated frames; arena exhaustion mid-chain will cause a null `cbm_scope_push` return, and the code in `go_lsp.c` handles this with `if (!scope) return current;` — so the frame stays the parent. This is correct for OOM, but it silently merges scopes, causing variable shadowing bugs rather than crashes. Document this explicitly as the intended degradation mode for py_lsp.

**No recursion depth guard for C3 MRO linearization.**

Phase 6 and 8 implement C3 linearization. The algorithm is recursive in most reference implementations. A pathological class hierarchy (100-deep diamond inheritance, common in some ORM frameworks) can stack-overflow the C process, not just exhaust the arena. The plan says nothing about a depth cap. The go_lsp has a depth-16 cap on alias resolution (`cbm_type_resolve_alias`). The same pattern (depth counter, bail to UNKNOWN at depth > N) must be applied to `go_lookup_field_or_method`-equivalent in py_lsp.

**The stdlib generator runs python3.9 per global preferences, but Jedi 0.20.0 requires Python 3.10+.**

The global CLAUDE.md prefers `python3.9`. The generator script is explicitly targeted at Python 3.9 (plan Phase 10). This is consistent. But if the developer uses Jedi as a dev-time oracle (Alternative 1 above), they will need a separate venv or the oracle won't import. This is an operational friction point worth calling out.

**No performance regression gate for Python specifically.**

Phase 12 targets <30% LSP overhead over baseline extraction. But the "baseline" for Python extraction is already fast (grammar_python.c is 3 lines — essentially zero LSP overhead today). The correct comparison is per-file LSP time in absolute terms, not relative to baseline. A 30% overhead on a 1ms baseline is only 0.3ms; 30% on a 50ms baseline is 15ms. Specify the absolute budget (e.g., "py_lsp must complete in <10ms for a 500-line file").

### Research Findings

- [TypeEvalPy (ICSE 2024): Precision/recall benchmarks for Pyright vs Jedi on Python type inference](https://arxiv.org/html/2312.16882v2)
- [Pyright pypi 1.1.407, latest release tracker](https://pypi.org/project/pyright/)
- [basedpyright: Pyright fork with PyPI install, no Node.js, biweekly rebase](https://github.com/DetachHead/basedpyright)
- [Pyrefly open-sourced May 2025, active alpha, >70% conformance](https://engineering.fb.com/2025/05/15/open-sourcing-pyrefly-a-faster-python-type-checker-written-in-rust/)
- [Jedi 0.20.0 released May 2026, drops Python 3.9, maintainer announces Zuban as successor](https://pypi.org/project/jedi/)
- [Zuban: Rust, AGPL-3.0, Jedi successor, 96.4% typing conformance, Sept 2025](https://github.com/zubanls/zuban)
- [Pyright LSP #10086: incomplete references without explicit didOpen for every file — fatal for batch resolver use](https://github.com/microsoft/pyright/issues/10086)
- [typeshed-client: versioned PyPI package for consuming stubs](https://pypi.org/project/typeshed-client/)
- [typeshed has no release tags, only continuous commits via automated PyPI publishing](https://github.com/python/typeshed)
- [Positron evaluated Pyrefly as default Python language server, March 2026](https://positron.posit.co/blog/posts/2026-03-31-python-type-checkers/)
- [tree-sitter-python v0.25.0 latest release (confirmed PEP 695 nodes in vendored parser.c)](https://github.com/tree-sitter/tree-sitter-python/releases)
- [How well do new Python type checkers conform? Pyrefly, ty, Zuban comparison 2026](https://sinon.github.io/future-python-type-checkers/)

### Dependency Version Audit

| Dependency | Current (Vendored/Pinned) | Latest Stable | Delta | Action |
|---|---|---|---|---|
| tree-sitter-python grammar | ABI version 15 (parser.c header), likely ~v0.21–0.23 based on symbol numbering | v0.25.0 (Sept 2024) | ~2 minor versions | Low risk. PEP 695 and PEP 634 nodes are present in vendored grammar. Upgrade if adding new Python syntax support, otherwise defer. |
| Pyright (reference, not shipped) | `main` depth=1, no tag | 1.1.407 | Unknown drift | Pin to tag `1.1.407` or a specific commit SHA. Document the SHA in `PYTHON_LSP_PLAN.md` and the generator header. Main moves ~weekly. |
| typeshed (reference, not shipped) | `main` depth=1, no tag | No release tags exist | Continuous drift | Pin to a specific commit SHA (e.g., `git rev-parse HEAD` at time of generation). Store SHA in generator file header. No PyPI package to pin; commit is the only stable anchor. |
| Jedi (secondary reference, dev-time) | `master` depth=1 | 0.20.0 (May 2026) | 0.19.2 → 0.20.0 | **WARNING:** 0.20.0 drops Python 3.9 support. If using as oracle, pin to `jedi==0.19.2` in a Python 3.9-compatible venv. If upgrading to 3.10+ venv, 0.20.0 is fine. |
| typeshed-client (alternative to raw ast parsing) | Not used | 2.8.2 | Not in plan | Consider using `typeshed-client` (PyPI, versioned, maintained) instead of raw `ast` parsing of typeshed `.pyi` files. It provides a stable API to enumerate stub definitions with version guards already parsed. |

### Questions for the Developer

1. **Scope frame limit:** Have you measured the 99th percentile binding count per function frame in a real Python corpus? 64 is adequate for Go; Python modules commonly have class bodies with 80+ field annotations. What is the upgrade path — static array to 128, or dynamic linked list of frames?

2. **Phase 11 gate:** Three LSPs (Go, C++, Python) now all write to `resolved_calls` with no consumer. What is the concrete plan to wire pass_calls.c? If there is no scheduled slot, the value proposition of this entire effort is theoretical. Should Phase 11 be made mandatory and moved to Phase 9 (immediately after cross-file Python is working)?

3. **typeEvaluator.ts scope for Phase 6:** Before committing to the full phase plan, have you spiked `getTypeOfMemberAccess` for a simple `obj.method()` call? That function in typeEvaluator.ts alone involves type narrowing, class type resolution, protocol matching, and generic substitution. Have you time-boxed how many C lines that takes to implement at acceptable quality?

4. **Typeshed generator stability:** The plan uses raw `ast` parsing of typeshed `.pyi` files. `typeshed-client` (PyPI, versioned, 2.8.2) already does this with version-guard handling. Why write the generator from scratch rather than using `typeshed-client` as the parsing layer and only writing the C-code emitter?

5. **Version target:** The plan says "Python 3.9 per global preference" for the generator script itself, but asks "pin to 3.12 or union-of-versions superset?" for the generated stubs. These are different concerns. The generator can run on Python 3.9 while emitting stubs that cover 3.9–3.13 version ranges. Is the intent to emit the 3.9-compatible subset only (safe but incomplete), or emit the full union with `#if`-style version guards in the C data?

6. **C3 recursion depth:** What depth cap will you put on MRO linearization? Diamond hierarchies in real Python (SQLAlchemy models, Django ORM, Zope Component Architecture) exceed 20 levels. At what depth do you bail to UNKNOWN, and is that documented as a known limitation?

7. **False positive policy:** The plan says "bail to UNKNOWN on `__getattr__`." What is the confidence threshold below which you refuse to emit a resolved call entry at all? The Go LSP has explicit confidence values but `pass_calls.c` does not filter by them (since it ignores `resolved_calls` entirely). Before wiring, you need a policy: what confidence floor makes it into an edge vs stays as UNKNOWN?

### Recommendation

The plan is architecturally sound and follows the existing patterns correctly, but it has three problems that should be fixed before work starts rather than discovered in Phase 6. First, **raise `CBM_SCOPE_MAX_BINDINGS` to at least 128** (or dynamic) before writing a single Phase 1 test — the current limit will produce silent failures on real Python code and all unit tests will pass anyway because they use small snippets. Second, **make Phase 11 mandatory and move it to immediately after Phase 9** — shipping a third language's LSP output into a dead buffer is not a meaningful milestone, and the longer wiring is deferred the larger the eventual coordination cost across three language implementations. Third, **pin Pyright and typeshed to specific commit SHAs** before mining them — `main depth=1` means the reference shifts under you within days and reproducibility of the generator output is lost; use `typeshed-client` as the parser layer in `gen-py-stdlib.py` rather than raw `ast` to get a versioned, maintained parsing API. The 93-test estimate is defensible for v1 but the plan should explicitly document which resolution failure classes are accepted (metaclass `__call__`, `__getattr__`, `setattr`-dynamic, `importlib.import_module` dynamic import) so the benchmark in Phase 12 can measure against a declared ceiling rather than an undefined optimum.
