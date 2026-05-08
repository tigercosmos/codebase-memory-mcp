# PHP LSP — Pre-flight (Phase 0)

Date: 2026-05-08
Status: complete; **Phase 1 conditionally justified — see §6**.

This artifact is required by §0 of `docs/PLAN_PHP_LSP_INTEGRATION.md`.
It exists to verify that the failing benchmark questions are
type-resolver-shaped before any C is written.

## 1. Reproduction setup

- Repo: `https://github.com/laravel/framework`, shallow clone at HEAD
  (May 2026).
- Path: `/tmp/lang-bench/laravel-framework-php`.
- Indexed via `index_repository`.
- Project key: `tmp-lang-bench-laravel-framework-php`.
- Index size: **39 767 nodes / 196 979 edges** (vs. BENCHMARK.md
  snapshot of 38 644 / 161 242 — the corpus has grown ~22 % in edges
  since the benchmark was recorded).

## 2. The two failing benchmark questions

From `docs/BENCHMARK.md:297-299`:

| Q  | Failure                                                                                |
|----|----------------------------------------------------------------------------------------|
| Q6 | `search_code(error)`: TODO/FIXME=0; "error" found 3× in workflow YAML.                 |
| Q8 | `trace_call_path(inbound)`: resolved to method variant (0 callers) instead of helper.  |

## 3. Q6 — text search — root cause

**Not type-resolver-shaped.** This is the `search_code` text-search
backend — a regex/grep-style search over file content, no AST involvement.
PHP-LSP would not touch this code path at all.

Probable causes (out of scope for this plan):

- The search may be excluding string-literal-only matches (PHP source
  has plenty of `"error"` literals).
- The TODO/FIXME=0 result is independent — Laravel's source genuinely
  has very few of those markers.

**Verdict: Phase 1 will not move Q6.** Score ceiling without separate
text-search work is 11/12 (92 %). That is still Tier 1.

## 4. Q8 — inbound trace — root cause (the interesting one)

The benchmark called `trace_call_path(function_name="value",
direction="inbound")`. The tool resolved `value` to a method variant
with 0 callers. Two distinct bugs sit on top of this single observation.

### 4.1 Tool-level disambiguation bug (not type-resolver-shaped)

`search_graph(name_pattern="^value$")` against the current PHP index
returns **11 distinct nodes** named `value`:

| Kind     | Qualified name fragment                                  | in_degree |
|----------|----------------------------------------------------------|----------:|
| Function | `Illuminate.Collections.helpers.value`                   | 103       |
| Method   | `Illuminate.Database.Eloquent.Builder.Builder.value`     | 16        |
| Method   | `Illuminate.Support.Once.Once.value`                     | 8         |
| Method   | `Illuminate.Support.Uri.Uri.value`                       | 8         |
| Method   | `Illuminate.Database.Query.Builder.Builder.value`        | 6         |
| Method   | `Illuminate.Support.Fluent.Fluent.value`                 | 2         |
| Method   | `Illuminate.Support.UriQueryString.UriQueryString.value` | 1         |
| Method   | `Illuminate.Support.Stringable.Stringable.value`         | 0         |
| Method   | `Illuminate.Support.Benchmark.Benchmark.value`           | 0         |
| Method   | `Illuminate.Collections.Traits.EnumeratesValues...value` | 0         |
| Variable | `types.Log.Context.value`                                | 0         |

When the trace tool was given the short name `value`, it picked a
variant with `in_degree = 0` and reported "no callers." This is a
**tool-level UX problem**: when multiple QNs match a short name,
`trace_call_path` should either (a) disambiguate by aggregating across
all matching short-name nodes, (b) prefer the highest-degree match, or
(c) ask the user to pick. None of those require PHP-LSP.

**Verdict: php_lsp.c does NOT directly fix Q8.** A separate trace-tool
fix does.

### 4.2 Attribution bug (real, type-resolver-shaped)

Querying who actually calls each `value` variant reveals
misattribution. The 103 inbound edges to the helper function include
several call sites that are actually `$receiver->value()` method calls
on a typed receiver, which the current extractor mis-routes to the
global helper because it cannot resolve the receiver type:

#### Concrete misattributed call sites

| Caller (currently routed to helper)                              | Actual code (verified)                                  | Should be                |
|------------------------------------------------------------------|---------------------------------------------------------|--------------------------|
| `ConfiguresPrompts::configurePrompts`                            | `$prompt->value()` × 3, where `$prompt: Prompt`         | `Prompt::value` (method) |
| `DatabaseQueue::creationTimeOfOldestPendingJob`                  | `... ->value('available_at')` (Builder chain end)       | `Builder::value`         |

Verified by `grep -n "value(" <file>`:

```
src/Illuminate/Console/Concerns/ConfiguresPrompts.php:
  34: ... fn (Prompt $prompt) => $this->validatePrompt($prompt->value(), ...);
  58: ... $this->components->ask($prompt->message, $prompt->value());
  60: ... return $prompt->value();

src/Illuminate/Queue/DatabaseQueue.php:
  236: ... ->value('available_at');
```

#### Across-file pattern count

```
$ grep -rn '->value(' src/ | wc -l   # method-style
30
$ grep -rn 'value('   src/ | wc -l   # all references including method-style
~300
```

Out of `200 - 300` raw `value(` occurrences, ~30 are clearly
method-style (`->value(`). The graph currently attributes 41 calls
to method variants (16 + 8 + 8 + 6 + 2 + 1) and 103 to the helper.
That implies the extractor is over-counting the helper by **roughly
20–30 %**, where those mis-routes are exactly the cases php_lsp.c
exists to fix: typed receiver, name collides with a global function.

This pattern generalises beyond `value()`. Any short name shared
between a global helper (Laravel has ~50 in `helpers.php`) and a method
on a Laravel class will exhibit the same misrouting.

**Verdict: php_lsp.c materially fixes the attribution bug, even
though it does NOT fix the disambiguation bug surfaced by Q8.**

## 5. Wider correctness signal (the real argument)

The benchmark question framing under-sells the actual correctness
problem. The Q8 investigation surfaced that the current PHP extractor
has systemic call-attribution drift on any short name that exists as
both a global function and a method. Concretely, the graph's PHP
section is **lossy in a structured way** that affects:

- inbound traces (the Q8 symptom),
- outbound traces from any caller that uses such short names (Q7
  passes for Laravel only because the spot-check called `value`'s
  outbound, where the helper happens to be the right answer),
- Cypher `CALLS` correctness (Q9 passes because the smoke check is
  shallow),
- any downstream feature that relies on accurate caller→callee
  attribution (impact analysis, dead-code, refactor blast radius).

This is the case for php_lsp.c as a graph-quality investment, not
purely as a benchmark-tier-lift investment.

## 6. Decision

**Phase 1 is justified, but the success metric must change.**

Original plan success metric: PHP benchmark from 75 % → ≥ 90 %.

Updated success metric (replace in `docs/PLAN_PHP_LSP_INTEGRATION.md`
§12 and §10):

1. **Primary**: a measurable attribution correctness lift on
   laravel/framework. Target — for the top-20 short names that collide
   between a `Function` and a `Method` (`value`, `make`, `get`, `set`,
   `find`, `first`, `where`, `count`, etc.), at least 90 % of call
   sites with a typed receiver land on the method variant, not the
   helper. Baseline measured pre-change; verified post-change.
2. **Secondary**: PHP benchmark Q8 PARTIAL → PASS *if and only if*
   the trace-tool disambiguation fix lands in the same release.
   Otherwise Q8 stays PARTIAL but the underlying graph is correct, and
   the tool-side fix is a separate ticket.
3. **Side effect**: Q6 stays PARTIAL until the search_code text-search
   fix lands separately. Cap the achievable benchmark lift at 11/12
   (92 %, still Tier 1) until both side fixes ship.

## 7. Knock-on plan changes

Apply these edits to `docs/PLAN_PHP_LSP_INTEGRATION.md`:

| Change                                                                 | Section          |
|------------------------------------------------------------------------|------------------|
| Re-frame primary success metric from "Tier 1 benchmark" to "attribution correctness on collide-set short names; benchmark lift is a side-effect" | §10, §12         |
| Add explicit non-goal: "fix `trace_call_path` short-name disambiguation" — separate ticket | §2 out-of-scope  |
| Add explicit non-goal: "fix `search_code` text-search recall" — separate ticket | §2 out-of-scope  |
| Add a "baseline measurement" step to §0: run the collide-set query before code lands and record the numbers in this file | §0               |
| Note in §10 acceptance: `tests/test_php_lsp.c` must include a fixture asserting `Prompt::value` is the resolved callee for `(Prompt $p) => $p->value()` | §10              |

## 8. Baseline measurements (collide-set, pre-change)

Run the same script post-Phase 1 and require strict improvement.

| Short name | Function in_degree | Top-method in_degree | Sum methods | Helper-share | Notes                       |
|------------|-------------------:|---------------------:|------------:|-------------:|-----------------------------|
| value      | 103                | 16 (Eloquent Builder)| 41          | 71.5 %       | spot-checked: ≥ 4 misroutes  |
| (TODO)     |                    |                      |             |              | extend in implementation pass |

Helper-share = `function_in_degree / (function_in_degree + sum_methods)`.
Lower is better post-change for cases where typed receivers exist.

The `(TODO)` row will be filled as part of the Phase 1 first-day work
(once `php_lsp.c` skeleton exists, run a dry-run extractor and diff).

## 9. Conclusion

- The plan's stated benchmark-lift hypothesis is partially wrong
  (Q6 is unrelated; Q8 is half tool-bug, half attribution-bug).
- The underlying motivation — *PHP graph correctness on real codebases*
  — is real, measurable, and validated by spot-check on Laravel.
- Phase 1 proceeds with a sharpened metric and explicit non-goals.

Phase 1 is unblocked.
