# Coverage-Tooling Compatibility

**pcov-enhanced compiles and loads as the `pcov` extension** (a drop-in
replacement), keeps line coverage byte-for-byte identical to upstream pcov, and
adds opt-in branch/path coverage under `pcov.mode=branch` that is consumable
through unmodified `phpunit --path-coverage`. This document surveys the
coverage-consuming tools in the PHP ecosystem and states, for each, how it
selects a coverage driver, whether line mode works, whether branch/path works,
and any caveats.

The key fact that governs every row below comes from the driver selection logic
in `SebastianBergmann\CodeCoverage`, verified by reading source in
[`01-contract.md`](01-contract.md) §2:

```php
// php-code-coverage src/Driver/Selector.php::select()
if ($granularity === Granularity::Line && $runtime->hasPCOV()) return new PcovDriver($filter);
if ($runtime->hasXdebug())                                     return new XdebugDriver(...);
throw new NoSupportedDriverAvailableException($granularity);
```

Two consequences follow, and they are the whole story:

1. **Line coverage** (`Granularity::Line`) is served by `PcovDriver` whenever
   `Runtime::hasPCOV()` is true. This build *is* pcov, so line coverage is
   selected natively and behaves exactly as upstream pcov.
2. **Branch/path coverage** (any non-`Line` granularity) skips pcov entirely and
   is routed to `XdebugDriver`, which first calls `ensureXdebugIsAvailable()`
   (`extension_loaded('xdebug')`, version `>= 3.1`, `'coverage'` in
   `xdebug_info('mode')`) and then the free `xdebug_*` coverage functions. The
   opt-in Xdebug-compat surface in `pcov_xdebug_compat.c` satisfies exactly those
   checks and answers those calls, so `--path-coverage` works with **unmodified**
   tooling. See [`02-integration-decision.md`](02-integration-decision.md).

Because virtually every higher-level tool below delegates coverage collection to
`php-code-coverage`, they all inherit this same driver-selection behaviour. The
tool does not pick a driver itself; php-code-coverage's `Selector` does.

## The real-Xdebug coexistence rule

This rule applies uniformly to **every** tool in the matrix:

> **If the real Xdebug extension is loaded, the pcov-enhanced Xdebug-compat
> surface does not register.** Real Xdebug wins for branch/path coverage, and
> pcov-enhanced downgrades to serving **line coverage only** (via `PcovDriver`
> at `Granularity::Line`). This avoids symbol clashes and double instrumentation.
> The shim is a stand-in for branch/path *only when Xdebug is absent*.

Practically: install exactly one branch/path provider. Use pcov-enhanced in
`pcov.mode=branch` for fast branch/path, **or** use real Xdebug. Do not expect
both to be active at once; when both are present, Xdebug provides branch/path and
pcov-enhanced quietly stays a line-coverage driver.

## Compatibility matrix

| Tool | How it selects a driver | Line mode | Branch/path mode | Caveats |
|------|-------------------------|:---------:|:----------------:|---------|
| **PHPUnit + php-code-coverage** | `Driver\Selector::select()` (see above); PHPUnit builds its own selector-chosen driver per run | ✅ `PcovDriver` (native pcov, unchanged from upstream) | ✅ via `--path-coverage` through the Xdebug-compat surface under `pcov.mode=branch` | Branch/path requires `pcov.mode=branch`; startup-only INI. Absolute branch/opcode ids differ from Xdebug but Branch%/Path% match (see below). |
| **Infection** (mutation testing) | Drives `php-code-coverage` to gather initial coverage before mutating; same `Selector` path | ✅ (line coverage is what Infection primarily needs) | ✅ inherited — if configured for path coverage it flows through the same shim | Infection's default coverage need is line-level; it inherits pcov line performance. No Infection-specific driver logic to satisfy. |
| **Paratest** (parallel PHPUnit) | Each worker is a PHPUnit process that runs `Selector::select()` independently; per-worker coverage is merged | ✅ per worker | ✅ per worker (each worker resolves the same shim) | Driver selection happens once per worker process; the coexistence rule applies per worker. Set `pcov.mode=branch` in the base INI so every worker inherits it (it is startup-only, not `.user.ini`-settable). |
| **Codeception** | Delegates coverage to `php-code-coverage` (its `Coverage` module wraps `CodeCoverage`) | ✅ | ✅ inherited via the same `Selector` | No bespoke driver; behaves like PHPUnit. Configure branch/path in Codeception's coverage settings; the extension must be in `pcov.mode=branch`. |
| **Behat** (+ behat coverage extensions such as `LeanPHP`/`dvdoug/behat-code-coverage`) | Delegates to `php-code-coverage` `CodeCoverage` | ✅ | ✅ inherited via the same `Selector` | Same as Codeception: no driver logic of its own; inherits pcov line / shim branch behaviour. |
| **Coveralls uploader** (`php-coveralls`) | Does **not** collect coverage; consumes the **Clover XML** php-code-coverage emits | ✅ driver-agnostic | ✅ driver-agnostic | Works unchanged regardless of which driver produced the report. Point it at the Clover file (`--coverage-clover`). |
| **Codecov uploader** | Does **not** collect coverage; consumes Clover/Cobertura XML | ✅ driver-agnostic | ✅ driver-agnostic | Works unchanged. Report format, not driver, is what it reads. |
| **Scrutinizer** | Does **not** collect coverage; consumes Clover XML uploaded via `ocular`/its runner | ✅ driver-agnostic | ✅ driver-agnostic | Works unchanged. Driver-agnostic by construction. |

Legend: ✅ = supported. "inherited" = the tool does not select a driver itself; it
uses `php-code-coverage`, so it gets exactly the PHPUnit row's behaviour.

### Why the uploaders are driver-agnostic

Coveralls, Codecov and Scrutinizer are **report consumers**, not coverage
collectors. They read the Clover (`--coverage-clover`) or Cobertura
(`--coverage-cobertura`) XML that `php-code-coverage` writes. That XML is
produced identically no matter whether the underlying driver was pcov,
pcov-enhanced's shim, or real Xdebug. As long as php-code-coverage can build a
report, the uploaders work unchanged. This repository's CI `coverage-reports`
job exercises exactly this: it runs the fixture suite under this build and emits
a Cobertura XML report (see below and [`04-distribution.md`](04-distribution.md)).

## Branch/path semantics under the shim

For tools that request branch/path coverage, the numbers php-code-coverage
reports depend only on branch/path **counts** and `hit >= 1`, not on the absolute
opcode/branch ids. As documented and validated in [`03-results.md`](03-results.md):

- On the required corpus pcov-enhanced matches Xdebug 3.6 exactly: total branches
  35 / 35, hit branches 35 / 35, total paths 21 / 21, hit paths 10 / 10.
- A representative class (`Calc`) reports 19 branches / 11 paths per method,
  identical to Xdebug; path explosion caps identically at 4096 paths.
- Absolute opcode indexes differ (pcov compiles op_arrays with
  `ZEND_COMPILE_NO_JUMPTABLES`), so branch **ids** are not identical to Xdebug's,
  but Branch% / Path% match. This is expected and documented, not a defect.

## Performance (why you would choose this over Xdebug for branch/path)

From the validated microbenchmark in [`03-results.md`](03-results.md) (PHP
8.4.24, branch-heavy workload, per iteration):

| Configuration | per iteration | vs no-coverage |
|---------------|--------------:|---------------:|
| no coverage | 0.026 ms | 1× |
| pcov line | 0.35 ms | 13× |
| **pcov branch** | **0.60 ms** | **23×** |
| Xdebug path | 243.8 ms | 9250× |

pcov branch coverage is **~406× faster than Xdebug path coverage** on that
microbenchmark and ~1.7× the cost of pcov line coverage. Every tool that routes
branch/path through the shim inherits this margin. (The microbenchmark is
recursion-heavy and thus flattering; the order-of-magnitude conclusion is robust.
Do not extrapolate the exact multiplier to arbitrary suites.)

## Validation status

This is a documentation deliverable; the compatibility claims are grounded as
follows, with **no fabricated tool output**:

- **Driver-selection claims** are read directly from php-code-coverage source and
  recorded in [`01-contract.md`](01-contract.md) §2 (the `Selector::select()`
  logic and `ensureXdebugIsAvailable()` checks).
- **End-to-end `phpunit --path-coverage`** producing non-zero Branches/Paths
  under this build is the **validation of record** in [`03-results.md`](03-results.md)
  §1 (PHP 8.5.10 and 8.4.24: Paths 60% (6/10), Branches 100% (18/18), Lines 100%
  (13/13)) and is exercised on every push by the CI `coverage-reports` job, which
  runs the `ci/report-project` fixture under this build and emits Markdown, JSON,
  a PHPUnit HTML report and a Cobertura XML (the uploader input). See
  [`../README.md`](../README.md) "Continuous integration & reports".
- **Offline note:** empirically re-running the `ci/report-project` fixture in this
  sandbox is not possible because `composer install` requires network access
  (the environment is repository-access-only and `vendor/` is not present).
  Therefore the CI `coverage-reports` job and [`03-results.md`](03-results.md) are
  cited as the validation of record for the end-to-end tooling flow, rather than
  re-run here. The `.phpt` suite (which does not need Composer) is run as a
  regression guard in both line and branch mode.

## See also

- [`01-contract.md`](01-contract.md) — the exact php-code-coverage driver
  contract and `Selector` logic.
- [`02-integration-decision.md`](02-integration-decision.md) — why route (c) (the
  Xdebug-compat surface) was chosen for the "works today" path.
- [`03-results.md`](03-results.md) — validated performance and semantic-equivalence
  numbers cited above.
- [`06-future-native-api.md`](06-future-native-api.md) — the forward-looking clean
  route (a): a native pcov branch/path driver selected without Xdebug impersonation.
- [`04-distribution.md`](04-distribution.md) — how to install this build in CI,
  Docker, Kubernetes, PECL/PIE.
