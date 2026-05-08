# PHP Light Semantic Pass (LSP) — Integration Plan

Branch: `worktree-php-lsp-integration`
Target file output: this document
Date: 2026-05-08

## What this is and isn't

**This is**: writing a PHP semantic analyzer in C, from scratch, as a new
sibling to the existing `go_lsp.c` and `c_lsp.c` modules. It runs
in-process, against our own tree-sitter AST, and emits resolved-call
edges in our existing format.

**This is not**: integrating, embedding, wrapping, or subprocessing any
existing PHP language server (phpactor, intelephense, php-language-server,
PHPStan, Psalm). No PHP runtime ships with the binary. No new third-party
code ends up linked in.

**Reference-only use of upstream PHP tooling**: `phpactor/worse-reflection`
is cloned to a scratch directory during development as an algorithmic
reference — we read its resolver code to understand how to handle, e.g.,
trait conflict resolution or `parent::` chain walking, then write the
equivalent in C. None of its source, dependencies, or runtime is included
in the build artifact.

**The only PHP-related third-party code in the binary** is the
tree-sitter-php grammar, which is already vendored, already compiled, and
already used by the unified extractor today. This plan adds zero new
dependencies.

The name "LSP" in this codebase means **Light Semantic Pass** (our
in-process type resolver), not Language Server Protocol. The naming
collision is unfortunate; see §1.

## 0. Pre-flight (status: complete)

Pre-flight executed; output recorded in `docs/PHP_LSP_PRE_FLIGHT.md`.
Key findings:

- Q6 (text search) is **not** type-resolver-shaped — separate ticket.
- Q8 (inbound trace) is **half** type-resolver-shaped: a real
  attribution bug exists (typed-receiver `$x->value()` calls misroute
  to the global `value()` helper because the receiver type is unknown)
  *and* a separate trace-tool disambiguation bug exists. PHP-LSP fixes
  the first; the second is a separate ticket.
- The wider correctness signal — systemic call-attribution drift on
  any short name shared between a global function and a method — is
  the real justification for Phase 1, validated by spot-check on
  Laravel.

Phase 1 proceeds with the sharpened success metric below (§10, §12).
A baseline measurement table for the collide-set is in
`docs/PHP_LSP_PRE_FLIGHT.md` §8 and must be re-measured post-change.

## 1. Terminology

In this codebase, **LSP = Light Semantic Pass**, not Language Server Protocol.
It is an in-process C module that runs after `cbm_extract_unified` in `cbm.c` to:

1. Build a `CBMTypeRegistry` of types/funcs from local + cross-file defs.
2. Walk the tree-sitter AST a second time, tracking lexical scopes (`CBMScope`)
   and per-variable/parameter types (`CBMType`).
3. Resolve member/static/function calls into precise callee QNs, emitting
   `CBMResolvedCall` rows with a strategy + confidence score.

Existing language LSPs:

| File                     | Lines | Lang        |
|--------------------------|------:|-------------|
| `internal/cbm/lsp/go_lsp.c` | 2 744 | Go          |
| `internal/cbm/lsp/c_lsp.c`  | 4 767 | C / C++ / CUDA |

This plan adds a third: `internal/cbm/lsp/php_lsp.c`.

## 2. Scope

In scope:

- New module `internal/cbm/lsp/php_lsp.{c,h}`.
- New stdlib data file `internal/cbm/lsp/generated/php_stdlib_data.c`
  (a curated subset, **not** full PHP stdlib — see §6).
- Wire into `internal/cbm/lsp_all.c`, `internal/cbm/cbm.c`, `Makefile.cbm`.
- New tests `tests/test_php_lsp.c` covering single-file and cross-file
  call resolution.
- Update `docs/BENCHMARK.md` after benchmark re-run.

Explicit non-goals (separate tickets, do not block Phase 1):

- **`trace_call_path` short-name disambiguation.** The Q8 benchmark
  failure is half tool-bug (picks one of 11 `value` variants and
  reports its zero callers). Fixing the tool is a separate change to
  the trace tool, not to PHP analysis. See
  `docs/PHP_LSP_PRE_FLIGHT.md` §4.1.
- **`search_code` text-search recall.** The Q6 benchmark failure
  ("error" found 3× in workflow YAML; TODO/FIXME=0) is text-search
  backend behaviour with no AST involvement. Separate ticket. See
  `docs/PHP_LSP_PRE_FLIGHT.md` §3.

Out of scope (deferred, with justification):

- Embedding phpactor / tolerant-php-parser. Reason: phpactor is 28 KLoC of
  PHP plus a fork of tolerant-php-parser; not embeddable into a C process
  short of bundling a PHP interpreter. Same arguments-against as for Go and
  C — the existing in-process C resolvers prove the pattern works.
- LSP wire-protocol exposure (i.e. exposing the codebase-memory-mcp graph
  to editors as a Language Server). Different feature, different name
  collision, separate plan.
- Generic/template tracking (`@template`, generic collections).
  Defer until single-file + cross-file basics are at parity with Go LSP.
- Full PHPDoc parsing. Phase 2 stretch goal — see §10.

## 3. Why this works in-process

The codebase already has every primitive needed:

| Need                                  | Existing primitive                                      |
|---------------------------------------|---------------------------------------------------------|
| Parse PHP                             | `tree_sitter_php_only` (vendored, compiled, registered) |
| Walk PHP AST with shared semantics    | `cbm_lang_spec(CBM_LANG_PHP)` already defines          |
|                                       | `php_func_types`, `php_class_types`, `php_call_types`,  |
|                                       | `php_import_types`, `php_decorator_types`, etc.         |
| Allocate transient state              | `CBMArena` (`internal/cbm/arena.h`)                     |
| Track variable bindings               | `CBMScope` (`internal/cbm/lsp/scope.h`)                 |
| Represent types                       | `CBMType` (`internal/cbm/lsp/type_rep.h`)               |
| Cross-file lookup                     | `CBMTypeRegistry` + `CBMLSPDef`                         |
| Emit resolved calls                   | `CBMResolvedCallArray`                                  |

PHP fits the existing model cleanly:

- Methods → `CBM_TYPE_NAMED` receiver + method name lookup, identical to Go.
- Static calls → resolved against `CBMRegisteredType` like C++ static methods.
- Namespaces → behave like Go packages: a `use` clause maps a local name
  (`Foo` or aliased `Foo as Bar`) to a fully-qualified namespace QN, exactly
  the role of `import_local_names` / `import_package_qns` in `GoLSPContext`.

## 4. Design

### 4.1 Module layout

```
internal/cbm/lsp/
├── php_lsp.h                         (~120 lines, new)
├── php_lsp.c                         (~2 500 lines, new)
└── generated/
    └── php_stdlib_data.c             (~200 lines, curated by hand)
```

### 4.2 `PHPLSPContext` (mirrors `GoLSPContext`)

```c
typedef struct {
    CBMArena              *arena;
    const char            *source;
    int                    source_len;
    const CBMTypeRegistry *registry;
    CBMScope              *current_scope;

    // Namespace + use-statement maps (PHP-specific)
    const char  *current_namespace_qn;       // "" for global namespace
    const char **use_local_names;            // local alias or last NS segment
    const char **use_target_qns;             // resolved target QN
    int          use_count;
    enum { USE_CLASS, USE_FUNCTION, USE_CONST } *use_kinds;  // PHP `use function` / `use const`

    // Current function/method context
    const char *enclosing_func_qn;
    const char *enclosing_class_qn;          // NULL outside class body
    const char *module_qn;                   // file-level QN

    CBMResolvedCallArray *resolved_calls;
    bool debug;
} PHPLSPContext;
```

The three PHP-specific fields versus `GoLSPContext` are: `current_namespace_qn`,
`enclosing_class_qn` (Go has none — PHP method body uses `$this`), and
`use_kinds` (PHP allows `use function` and `use const`, separate import
namespaces).

### 4.3 Pass structure

`cbm_run_php_lsp(arena, result, source, source_len, root)` follows the
go_lsp.c shape exactly:

1. Build per-file registry from `result->definitions` (already populated
   by the unified extractor) + stdlib data.
2. Walk top-level: collect `namespace` declaration, `use_declaration`
   nodes, populate `use_*` arrays.
3. Walk each function/method body, push scope, bind parameters with their
   declared types, recurse.
4. For each call expression, evaluate receiver type (if any), look up the
   method/function in the registry, emit a `CBMResolvedCall`.

### 4.4 Resolver coverage (Phase 1)

| AST node type                            | Resolver                                                              |
|------------------------------------------|-----------------------------------------------------------------------|
| `function_call_expression`               | global function lookup, namespaced via `use function` and current NS  |
| `member_call_expression` (`$x->m()`)     | type of `$x` → method on receiver type or its parents                 |
| `nullsafe_member_call_expression`        | same, drop nullable                                                   |
| `scoped_call_expression` (`A::m()`, `self::m()`, `parent::m()`, `static::m()`) | static method on resolved class QN                |
| `object_creation_expression` (`new Foo`) | type = `Foo` (resolved via `use` + current NS)                        |
| `assignment_expression`                  | bind LHS variable name to RHS evaluated type                          |
| `parameter` / `simple_parameter` (typed) | bind `$param` to declared type                                        |
| `property_declaration` (typed)           | record field type on `CBMRegisteredType`                              |
| `property_promotion` (constructor)       | both bind in scope and record field                                   |
| `return_statement`                       | unify with declared return type for return-type inference fallback    |
| `catch_clause`                           | bind `$e` to caught exception type                                    |
| `foreach_statement`                      | bind loop var(s) to element/key types when array `@var` shape known   |
| `match_expression` arm result            | tuple-style return, used when assigned                                |
| `static_call_expression` to `parent::`   | walk inheritance chain to first method match                          |
| `enum_declaration` cases                 | register as named-type members                                        |
| `trait_declaration` + `use_declaration` (inside class body) | flatten trait methods into `CBMRegisteredType`     |

Phase 1 explicitly **excludes**: closures bound via `Closure::bind`,
variadic spread argument typing, generic `@template` PHPDoc, intersection
types (treated as union for resolver, exact treatment Phase 2),
DNF (disjunctive normal form) types beyond union/intersection (PHP 8.2+).

### 4.5 Confidence + strategy strings

To match what Go/C LSP emit so existing pipeline stages don't need new
strategy enums:

| Strategy                       | Confidence | Used when                                                |
|--------------------------------|-----------:|----------------------------------------------------------|
| `php_method_typed`             | 0.95       | receiver type known, method found on type                |
| `php_method_inherited`         | 0.90       | method found on parent class via hierarchy walk          |
| `php_method_trait`             | 0.90       | method found via flattened trait                         |
| `php_static_resolved`          | 0.95       | `Class::m()` with class QN resolvable                    |
| `php_self_static`              | 0.95       | `self::m()` / `static::m()` inside known class           |
| `php_function_namespaced`      | 0.95       | function in current namespace or via `use function`      |
| `php_function_global_fallback` | 0.70       | function not found in current NS, falls back to root NS  |
| `php_method_unresolved`        | 0.30       | receiver type unknown, name match only                   |
| `php_method_dynamic`           | 0.20       | receiver resolved to class with `__call`, method not in registry |
| `php_dynamic_unresolved`       | 0.10       | static call to class with `__callStatic` (e.g. Laravel facade) |

## 5. File-by-file change list

| Path                                                 | Change           | Est. lines       |
|------------------------------------------------------|------------------|-----------------:|
| `internal/cbm/lsp/php_lsp.h`                         | new              | ~120             |
| `internal/cbm/lsp/php_lsp.c`                         | new              | 2 500–3 500 (3 500 hard gate) |
| `internal/cbm/lsp/php_composer.{c,h}`                | new (Phase 1)    | ~200             |
| `internal/cbm/lsp/generated/php_stdlib_data.c`       | new (corpus-seeded) | ~200          |
| `internal/cbm/lsp_all.c`                             | +2 includes      | +2               |
| `internal/cbm/cbm.c`                                 | dispatch         | +4               |
| `Makefile.cbm`                                       | none expected    | 0 (covered by `lsp_all.c`) |
| `tests/test_php_lsp.c`                               | new              | ~1 500           |
| `Makefile.cbm` test wiring (`TEST_PHP_LSP_SRCS`)     | +3               | +3               |
| `docs/BENCHMARK.md`                                  | rerun + update   | ~30 line edit    |

The dispatch insertion in `cbm.c` is exactly:

```c
if (language == CBM_LANG_PHP) {
    cbm_run_php_lsp(a, result, source, source_len, root);
}
```

Inserted between the existing Go LSP and C LSP `if` blocks at
`internal/cbm/cbm.c:355–360`.

## 6. Stdlib data strategy (corpus-driven)

Go LSP ships a 30 630-line auto-generated `go_stdlib_data.c`. We will
**not** mirror that for PHP. Strategy is corpus-driven, not taxonomy-
driven:

1. **Seed from the benchmark corpus.** Query the existing
   `laravel/framework` index for the top-N most-connected named
   types/interfaces by inbound CALLS edge count. The current data shows
   `Illuminate\Database\Eloquent\Model` (in=836),
   `Illuminate\Database\Schema\Blueprint` (in=785),
   `Illuminate\Container\Container` (in=625), and `Collection` (via
   `value()`, in=1 024). Top ~30 types like these go in first.
2. **Add the genuinely-stdlib base.** SPL container/iterator hierarchy
   (`Iterator`, `Generator`, `Countable`, `Traversable`,
   `IteratorAggregate`, `ArrayIterator`, `SplStack`, `SplQueue`,
   `SplObjectStorage`, `ArrayObject`); PSR interfaces (`Psr\Log\…`,
   `Psr\Container\…`, `Psr\Http\Message\…`); DateTime /
   DateTimeImmutable / DateInterval; `Throwable` / `Exception` / `Error`
   chain — the last is needed for catch-clause type binding.
3. **Document each entry.** One-line comment with the source (corpus
   metric or stdlib taxonomy) so the file is auditable.

Stays hand-curated, but seeded from real usage data. Re-evaluate if
benchmark shows specific classes missing — easy add. Auto-generation can
come later (Phase 2 stretch) if the maintenance burden grows.

## 7. Build + dependency changes

No new third-party dependencies. The only library changes:

- `lsp_all.c` adds `#include "lsp/php_lsp.c"` and
  `#include "lsp/generated/php_stdlib_data.c"`.

The build already compiles `grammar_php.c` (`internal/cbm/grammar_php.c`)
and links `tree_sitter_php_only`. No `Makefile.cbm` changes needed for the
production binary.

For tests, mirror the existing `test_go_lsp` / `test_c_lsp` wiring:

```make
TEST_PHP_LSP_SRCS = tests/test_php_lsp.c
ALL_TEST_SRCS    += $(TEST_PHP_LSP_SRCS)
```

## 8. Dependency Version Audit

Scope: dependencies that would be touched if we added third-party PHP
parsers/analyzers. For the chosen in-process design, **none** of these are
added — table is here for the record so that the choice "no new deps" is
auditable.

| Dependency                       | Latest stable (May 2026) | Action     | Reason                                                                    |
|----------------------------------|--------------------------|------------|---------------------------------------------------------------------------|
| `tree-sitter-php` grammar        | already vendored         | none       | grammar already compiled and exposed via `tree_sitter_php_only`           |
| `phpactor/worse-reflection`      | n/a (PHP, MIT)           | reference only | algorithmic reference; not embedded                                  |
| `nikic/PHP-Parser`               | n/a (PHP, BSD-3)         | reject     | duplicates tree-sitter-php for our purposes; would require PHP runtime    |
| `microsoft/tolerant-php-parser`  | n/a (PHP, MIT)           | reject     | same as above                                                             |
| `php-language-server` (felix-becker) | unmaintained         | reject     | dead since 2018; LSP-protocol scope, not what we need                     |
| `intelephense` / phpstorm        | proprietary              | reject     | license incompatible                                                      |

Required existing primitives in the codebase verified present:
`CBMArena`, `CBMScope`, `CBMType*`, `CBMTypeRegistry`, `CBMResolvedCall`,
`CBMLangSpec` for PHP (`internal/cbm/lang_specs.c:1542`),
`tree_sitter_php_only` (`internal/cbm/grammar_php.c`).

## 9. Test plan

### 9.1 Unit tests (`tests/test_php_lsp.c`)

Patterned after `tests/test_go_lsp.c` (98 tests). Target ~80 tests.

Single-file (≈55):

- Param type inference: typed param, untyped param fallback, nullable,
  union, intersection, DNF, `self`/`static`/`parent` types, scalar +
  primitive types (`int|string|bool|float|null|true|false|array|callable|iterable|object|never|void|mixed|string`).
- Method chaining: `$db->select()->where()->limit()->get()`.
- Static dispatch: `Foo::bar()`, `self::bar()`, `parent::bar()`,
  `static::bar()`.
- `new` object creation + immediate method call: `(new Foo())->bar()`.
- Constructor property promotion (`public function __construct(public Foo $foo) {}`).
- Trait method flattening.
- Enum methods + `Enum::Case`.
- Catch binding: `catch (RuntimeException $e) { $e->getMessage(); }`.
- Foreach over `iterable<T>` PHPDoc — accept failure in Phase 1.
- Match expression assignment.
- Closure/arrow function variable capture.
- Namespace resolution: `use App\Foo;` then `new Foo()`,
  `use App\Foo as F;` then `new F()`,
  `use function App\bar;`, `use const App\BAZ;`.
- Global namespace fallback for builtin functions (`strlen`, `array_map`).

Cross-file (≈25):

- Method dispatch across files (file A defines `Foo::bar()`, file B
  calls it via `$foo->bar()`).
- Interface dispatch (file A defines interface, file B implements,
  file C calls method via interface variable).
- Trait flattening across files (trait in file A, used in class in file B).
- Inheritance chain (parent in file A, child in file B, call resolves to
  parent method).
- Stdlib `DateTime` chain across files.
- PSR LoggerInterface dispatch.

### 9.2 Integration tests

Re-run the language benchmark in `docs/BENCHMARK.md` against
`laravel/framework`. Target: lift PHP from 9/12 (Tier 2, 75 %) to ≥11/12
(Tier 1, ≥92 %). The two questions PHP currently fails (per BENCHMARK.md
inspection task) are method-resolution-dependent, so this is a direct
test of the new module.

### 9.3 CI hooks

Existing build/test pipeline (`./build-all.sh`) already covers
`tests/test_go_lsp` and `tests/test_c_lsp` — adding `tests/test_php_lsp.c`
plugs into the same harness via `ALL_TEST_SRCS`.

### 9.4 Memory-safety + race tests

The Go and C LSP modules are exercised under `-fsanitize=address,thread`
in the existing test binaries (`GRAMMAR_CFLAGS_TEST` / `_TSAN`). PHP LSP
inherits the same compile flags by virtue of using the same arena +
shared scope/type primitives — no additional memory-safety wiring needed.

## 10. Phasing

**Phase 0** — Pre-flight, 1 day. Output `docs/PHP_LSP_PRE_FLIGHT.md`
(see §0). Gate on type-resolver-shaped failure set.

**Phase 1** — Core resolver, **≈3 weeks** of focused work.

- `php_lsp.{c,h}` covering §4.4 table. Hard gate: 3 500 lines for
  `php_lsp.c`. If hit, defer DNF and trait conflict-resolution rules to
  Phase 2.
- `php_composer.{c,h}` parser for `composer.json` `autoload.psr-4` and
  `vendor/composer/autoload_psr4.php` (when present). Pre-populates the
  type registry with namespace→file mappings before the LSP walk.
- Corpus-seeded stdlib (§6).
- ~80 unit tests + 2 PSR-4 parser tests, ≥ 85 % line coverage of
  `php_lsp.c`, ≥ 40 % test:code line ratio.
- Facade / dynamic strategy emission (`php_dynamic_unresolved`,
  `php_method_dynamic`).
- **Primary metric**: collide-set attribution correctness on
  laravel/framework (per `docs/PHP_LSP_PRE_FLIGHT.md` §6.1). For the
  top-20 names that exist as both `Function` and `Method`, ≥ 90 % of
  call sites with a typed receiver land on the method variant.
- **Secondary metric**: PHP benchmark lift. Cap is 11/12 (92 %, Tier 1)
  unless the Q6 search-recall and Q8 trace-disambiguation tickets ship
  in the same release.
- Land before any of Phase 2.

**Phase 2** — Type system depth, after Phase 1 lands.

- PHPDoc parser (just `@var`, `@param`, `@return`, `@template`).
- Generic / template propagation for collection types.
- DNF type handling on call dispatch.
- Auto-generated stdlib if benchmark shows recall gaps.

**Phase 3** — Cross-cutting nice-to-haves.

- `Closure::bind` / `Closure::fromCallable` resolution.
- Magic methods (`__call`, `__callStatic`) — usually unresolvable, emit
  with low confidence + reason.
- Attribute (`#[…]`) → method binding (route/handler discovery), if it
  proves valuable for the existing route extraction pipeline.

## 11. Risks

| Risk                                                                      | Mitigation                                                                                                |
|---------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------|
| `php_lsp.c` grows beyond go_lsp.c size and becomes unmaintainable         | Mirror go_lsp.c's static-helper conventions; lift shared sub-routines into `lsp/scope.c` if they recur    |
| Cross-file resolution misses because of PHP's `autoload` runtime nature   | We resolve by static `use`-clause + namespace; document the assumption in module header. Composer PSR-4 mapping is a Phase 3 stretch |
| Tree-sitter-php node naming drifts on upgrade                             | Pin grammar via vendoring (already done); add a smoke test that asserts the PHP node-type names we depend on are still emitted |
| PHPDoc-only typed code (very common in legacy PHP) gets ~0 resolution     | Phase 2 handles PHPDoc; document Phase 1 limitation in the module README so users understand              |
| Stdlib gaps cause benchmark misses                                        | Stdlib is hand-curated, easy to extend; keep a TODO list of misses surfaced by the bench                  |
| Memory-safety regressions vs sanitizer                                    | Run `make test-asan test-tsan` (existing targets) before merge; add to PR template                        |

## 12. Acceptance criteria

- [x] Pre-flight (§0) complete; root causes recorded in
      `docs/PHP_LSP_PRE_FLIGHT.md`; type-resolver-shaped subset
      confirmed.
- [ ] Collide-set attribution: ≥ 90 % of typed-receiver method-call
      sites for the top-20 collide-set short names route to the method
      variant, not the global function.
- [ ] Specific regression: `(Prompt $p) => $p->value()` resolves to
      `Prompt::value`, not `Illuminate\Collections\helpers.value`.
      Encoded as a unit test in `tests/test_php_lsp.c`.
- [ ] All Phase 1 tests pass under `-O2`, ASan, and TSan.
- [ ] `./build-all.sh` exits 0 with no new warnings.
- [ ] PHP benchmark in `docs/BENCHMARK.md` updated. Score lift target
      is 11/12 (92 %, Tier 1) once trace-tool disambiguation also
      ships; PHP-LSP alone may show 10/12 (83 %) in isolation, which
      is acceptable for this PR.
- [ ] Coverage of `internal/cbm/lsp/php_lsp.c` ≥ 85 % per existing
      coverage harness.
- [ ] Test:code line ratio ≥ 40 % (`wc -l tests/test_php_lsp.c
      internal/cbm/lsp/php_lsp.c`).
- [ ] `php_lsp.c` ≤ 3 500 lines; if exceeded, scope cuts documented.
- [ ] `php_composer.c` parses Laravel's `composer.json` correctly in a
      regression test.
- [ ] Facade strategy verified: `DB::table()` against Laravel emits
      `php_dynamic_unresolved`, not zero edges.
- [ ] No new third-party dependencies added.
- [ ] `tests/test_php_lsp.c` referenced from `Makefile.cbm`
      `ALL_TEST_SRCS`.

## 13. Challenger Review (full, verbatim)

The plan was reviewed by a separate challenger pass. The headline findings:

1. **Critical factual gap.** The plan claimed the two benchmark PARTIAL
   scores (Q6 text search, Q8 inbound trace) are method-resolution-shaped.
   They are not. Shipping the new module will not move PHP from 75 % to
   Tier 1 unless the actual failing questions are something the resolver
   touches. **A debug-mode benchmark must be re-run on the current
   binary before any C is written**, and the failing-question set
   recorded here. Phase 1 is gated on that data.
2. **Facade / `__callStatic` calls are not addressed.** Laravel's
   `DB::table()`, `Cache::get()`, `Auth::user()`, etc. resolve at runtime
   via `__callStatic` → container. With static `use`-only resolution,
   these produce zero edges silently. Action: emit a
   `php_dynamic_unresolved` strategy (confidence 0.10) for any call
   where the receiver resolves to a class that contains a `__call` or
   `__callStatic` magic method, so the call site shows up in the graph
   even if the callee is unknown. Add this row to §4.5.
3. **Composer PSR-4 parsing is too valuable to defer.** Reading
   `composer.json` `autoload.psr-4` (and `vendor/composer/autoload_psr4.php`
   if present) lifts cross-file resolution from "files that include `use`"
   to "every class in the project + vendored deps." This is how PHPStan
   and Psalm work without running PHP. Cost: ~200 lines of JSON parsing.
   Promoted from Phase 3 to **Phase 1**.
4. **Stdlib should be corpus-driven, not taxonomy-driven.** The
   benchmark corpus already exposes the high-leverage types: `Model`
   (in=836), `Blueprint` (in=785), `Container` (in=625), `Collection`
   (in=1024 via `value()`). Hand-curating SPL + PSR while leaving these
   out is a recall miss. Bootstrap the 200-line file from the top-N most-
   connected types in the corpus, then add SPL/PSR.
5. **Effort estimate is optimistic.** 2 weeks for ≈2 500 lines of C is
   tight. C-LSP is 4 767 lines for a simpler-typed language. PHP has
   union/intersection/DNF types, traits with conflict resolution, enums,
   nullsafe chains, `self`/`static`/`parent`. Re-target as **3 weeks**
   with a 3 500-line hard gate for `php_lsp.c`; if the gate is hit,
   defer DNF and trait conflict-resolution rules to Phase 2.
6. **Test:code ratio is tight but acceptable.** C-LSP is 31 %; we plan
   ~43 % (1 500 / 3 500). Hold ≥ 40 % at merge time.
7. **Strategy table has a gap.** There is no
   `php_method_dynamic` for "receiver resolved, method not in registry"
   (e.g., methods only existing via `__call`). Add it explicitly to §4.5
   with confidence 0.20.
8. **Tree-sitter-php grammar version is current** (v0.24.2,
   2024-08-18); runtime is v0.25.8 vs upstream v0.26.8. Runtime upgrade
   is out of scope for this plan but worth a follow-up ticket.

### Challenge → plan diffs

| # | Challenge                                  | Plan change                                                                                       |
|---|--------------------------------------------|---------------------------------------------------------------------------------------------------|
| 1 | Failing-question set unverified            | New §0 "Pre-flight" added below; **blocks Phase 1**                                              |
| 2 | Facade calls silently miss                 | Add `php_dynamic_unresolved` row to §4.5                                                          |
| 3 | Composer PSR-4 deferred too aggressively   | Promote PSR-4 parser to Phase 1; +200 lines, +2 unit tests                                        |
| 4 | Stdlib taxonomy-driven                     | §6 reframed as corpus-driven seed list                                                            |
| 5 | 2-week estimate optimistic                 | §10 timeline updated to 3 weeks; 3 500-line hard gate added                                       |
| 6 | Test ratio                                 | Acceptance criteria §12 includes ≥ 40 % test:code ratio                                           |
| 7 | Strategy gap                               | Add `php_method_dynamic` to §4.5                                                                  |
| 8 | Runtime version note                       | Recorded in §8 audit; no plan change                                                              |

(The plan body above already incorporates these diffs.)

## 14. References

Algorithmic references (read-only, not embedded):

- phpactor/worse-reflection — `src/Core/Inference/Resolver/` is the
  single best reference for resolver-by-resolver coverage. 41 resolver
  classes; we collapse them into one C dispatch in `php_lsp.c` since C
  doesn't pay for the indirection.
  License: MIT.
- phpactor/phpactor — the LSP server. We reference its `WorseReflection`
  bindings only to confirm node-type assumptions are correct.
- nikic/PHP-Parser — used as a sanity check for unusual syntax shapes
  (PHP 8.4 property hooks, asymmetric visibility); we do not pull in
  code, only docs.

In-tree references:

- `internal/cbm/lsp/go_lsp.c` — primary structural template.
- `internal/cbm/lsp/c_lsp.c` — secondary template, shows how to handle
  inheritance chains and namespace-like scopes.
- `internal/cbm/lang_specs.c:1542` — existing PHP language spec; do not
  modify in Phase 1.
- `internal/cbm/cbm.c:340–360` — dispatch site; one-block insertion.
