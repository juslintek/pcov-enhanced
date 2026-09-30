# Forward-Looking Design — A Native pcov Branch/Path API

The Xdebug-compat surface (`pcov_xdebug_compat.c`, route (c) in
[`02-integration-decision.md`](02-integration-decision.md)) exists so that
**unmodified** `phpunit --path-coverage` works *today* against a build that is
internally pcov. It does so by presenting an Xdebug-shaped interface
(`xdebug_*` coverage functions, `XDEBUG_CC_*` constants, `phpversion('xdebug')`
= `3.99.0-pcov`). That is a pragmatic bridge, not the honest long-term home.

This document sketches the clean alternative — **route (a)** — where
`php-code-coverage` learns to select a *native* pcov branch/path driver via a
capability probe, with no Xdebug impersonation, plus a proposal for a
faster/leaner native collection API and the infrastructure changes each
ecosystem would need to adopt it.

> **Line-mode invariant (holds throughout this document).** Nothing proposed
> here changes line coverage. Line coverage is, and remains, served by
> `PcovDriver` at `Granularity::Line` via `Runtime::hasPCOV()`, byte-for-byte
> identical to upstream pcov. Every proposal below is *additive* and gated on an
> explicit branch/path request. If none of it lands, line coverage is unaffected.

## (a) The clean route: a Selector capability-probe + native `PcovBranchDriver`

Recall the selection logic verified in [`01-contract.md`](01-contract.md) §2:

```php
// php-code-coverage src/Driver/Selector.php::select() — today
if ($granularity === Granularity::Line && $runtime->hasPCOV()) return new PcovDriver($filter);
if ($runtime->hasXdebug())                                     return new XdebugDriver(...);
throw new NoSupportedDriverAvailableException($granularity);
```

Branch/path granularity unconditionally falls through to `XdebugDriver`. Route
(c) works only because the compat surface makes `hasXdebug()` and
`ensureXdebugIsAvailable()` pass. Route (a) removes that need.

### Step 1 — a pcov capability probe

Expose, from the extension, an explicit, honest capability signal that
php-code-coverage can test *without* pcov pretending to be Xdebug. Two
compatible options, pick one (or ship both):

- A boolean feature function:
  ```php
  function_exists('pcov\\collect_path_coverage')
  ```
  Present only when the build supports branch/path collection (i.e. when built
  with the branch producer). Cheap, no new surface beyond one function name.

- A structured capability bitmask, richer and future-proof:
  ```php
  namespace pcov;
  const CAP_LINE   = 1 << 0;   // always set (upstream pcov)
  const CAP_BRANCH = 1 << 1;   // this build, in branch-capable mode
  const CAP_PATH   = 1 << 2;   // this build, in branch-capable mode
  function capabilities(): int; // returns CAP_LINE [| CAP_BRANCH | CAP_PATH]
  ```
  `Runtime` (in `sebastian/environment`) would gain a matching accessor, e.g.
  `Runtime::pcovCanCollectBranchAndPath(): bool`, implemented as
  `function_exists('pcov\\capabilities') && (\pcov\capabilities() & (\pcov\CAP_BRANCH | \pcov\CAP_PATH))`.

Both are honest: pcov advertises what pcov can do, under the `pcov\` namespace.
No `xdebug_*` symbols, no `phpversion('xdebug')` sentinel.

### Step 2 — the Selector patch

```php
// php-code-coverage src/Driver/Selector.php::select() — proposed
if ($granularity === Granularity::Line && $runtime->hasPCOV()) {
    return new PcovDriver($filter);
}

// NEW: native pcov branch/path, no Xdebug impersonation required.
if ($runtime->pcovCanCollectBranchAndPath()) {
    return new PcovBranchDriver($filter, $granularity);
}

if ($runtime->hasXdebug()) {
    return new XdebugDriver($filter, $granularity);
}

throw new NoSupportedDriverAvailableException($granularity);
```

Placement matters and preserves the line-mode invariant: the `Granularity::Line`
+ `hasPCOV()` branch is still first and unchanged, so line coverage keeps going
to `PcovDriver`. The new native branch/path clause sits *before* the Xdebug
fallback, so where pcov can serve branch/path natively it is preferred, and real
Xdebug remains the fallback when pcov is not branch-capable.

### Step 3 — a native `PcovBranchDriver`

The driver is thin because the **route-independent producer already exists** in
`pcov_branch.c` and `\pcov\collect()` already returns the
`{lines, functions:{branches,paths}}` shape in branch mode (see
[`03-results.md`](03-results.md) "What was built"). The driver only has to call
the native API and hand php-code-coverage its `RawCodeCoverageData`:

```php
namespace SebastianBergmann\CodeCoverage\Driver;

final class PcovBranchDriver extends Driver
{
    public function __construct(private Filter $filter, private Granularity $granularity) {}

    public function start(): void
    {
        // native pcov, no XDEBUG_CC_* constants
        \pcov\start();
    }

    public function stop(): RawCodeCoverageData
    {
        // \pcov\collect() in branch mode already returns the
        // {lines, functions:{branches,paths}} array php-code-coverage consumes.
        $raw = \pcov\collect(\pcov\all);
        \pcov\clear();

        return RawCodeCoverageData::fromXdebugWithPathCoverage($raw); // same shape, same constructor
    }

    public function nameAndVersion(): string
    {
        return 'pcov ' . phpversion('pcov'); // honest identity, no xdebug sentinel
    }
}
```

Because the array shape is *already* the one
`RawCodeCoverageData::fromXdebugWith{Branch,Path}Coverage()` expects (that is
what the producer was built to emit — see [`01-contract.md`](01-contract.md) §1a),
no data reshaping is required in the interim. The only new code upstream is the
`Selector` clause, the `Runtime` accessor, and this ~30-line driver.

**Net effect:** with route (a) landed, `phpunit --path-coverage` selects a driver
that is *honestly pcov*. The Xdebug-compat surface becomes unnecessary and can be
deprecated (see the migration plan). Line mode is untouched.

## (b) A faster / leaner native collection API

Route (a) as sketched above still routes through the *Xdebug-shaped intermediate*
array (`fromXdebugWithPathCoverage()`), because that is what php-code-coverage
consumes today. That intermediate is a large associative structure built in PHP
userland-visible form on every `collect()`. Once php-code-coverage has a native
pcov driver, we can offer a lower-overhead path that avoids building the
Xdebug-shaped array at all.

### Proposal

1. **Direct branch/path accessor.** A dedicated
   `\pcov\collect_path_coverage(int $type = \pcov\all, array $filter = []): array`
   that returns branch/path data in pcov's own compact representation, skipping
   the Xdebug-key naming (`op_start`/`op_end`/`out`/`out_hit`) where the consumer
   does not need it. The consumer opts into the leaner shape via the capability
   negotiation below.

2. **Capability negotiation.** The driver and extension agree, at `start()`
   time, on the richest representation both understand:
   - if the consumer advertises support for pcov's compact shape, the extension
     emits that directly (fewer allocations, no string keys per branch);
   - otherwise it falls back to the Xdebug-shaped array (route (a) baseline),
     preserving compatibility.

3. **Aggregated / streaming collection.** pcov already tracks per-invocation
   branch-start sequences and computes branch/path hits at `collect()` time
   (see [`03-results.md`](03-results.md) "Runtime hit tracking is deferred and
   cheap"). A native consumer can request *aggregated counters* (branch hit /
   path hit as integers) rather than the full topology on every stop, so
   incremental/long-running collectors avoid rebuilding the whole array
   repeatedly.

### Perf rationale

The runtime hook is already the cheap part: [`03-results.md`](03-results.md)
measures pcov branch at **0.60 ms/iter** vs Xdebug path at **243.8 ms/iter**
(~406× faster), and only ~1.7× the cost of pcov line (0.35 ms/iter). The
remaining avoidable cost is on the *collection/marshalling* side — building the
Xdebug-shaped associative array in `collect()`. Removing that intermediate for
native consumers is a pure marshalling win: it does not touch the hot per-opcode
path, so it cannot regress line mode or the branch runtime, and it shrinks the
per-`collect()` allocation footprint. This keeps pcov honest to krakjoe's stated
performance invariant — "don't turn pcov into a debugger"
([`01-contract.md`](01-contract.md) §3) — while giving path coverage an even
leaner ceiling than the shim.

> **Line-mode invariant.** The leaner API is gated on a branch/path request and
> capability negotiation. Line coverage never enters this path; `\pcov\collect()`
> in line mode returns the same bare `[file => [line => status]]` map as upstream.

## (c) Infrastructure adoption requirements

For the native path to become the default way tools get fast branch/path
coverage, each ecosystem layer needs a specific, small change:

| Layer | What it needs to change | Notes |
|-------|-------------------------|-------|
| **php-code-coverage** | Add the `PcovBranchDriver`, the `Selector` clause, and consume the capability probe. Optionally the leaner native shape from (b). | The ~30-line driver + one `Selector` clause above. Needs Sebastian Bergmann's review and a release. |
| **sebastian/environment (`Runtime`)** | Add `pcovCanCollectBranchAndPath()` (and, for (b), a shape-capability accessor) built on `\pcov\capabilities()` / `function_exists()`. | Small, self-contained; ships with php-code-coverage's dependency bump. |
| **pcov extension (this project)** | Ship `\pcov\capabilities()` / `\pcov\collect_path_coverage()` as first-class native API (already have the producer). Keep the Xdebug-compat surface during the transition, then deprecate it. | No new analysis code — reuses `pcov_branch.c`. Just new honest entry points. |
| **PHPUnit** | Nothing structural — PHPUnit builds its driver via php-code-coverage's `Selector`, so it inherits the new driver once php-code-coverage ships it. | Version-constraint bump on php-code-coverage only. |
| **setup-php** (`shivammathur/setup-php`) | A `coverage: pcov-branch` (or documented recipe) that installs this build in `pcov.mode=branch` without also installing Xdebug. | Today `coverage: pcov` installs upstream pcov (line only). See the recipe in [`04-distribution.md`](04-distribution.md). |
| **Distro / PIE / PECL** | Ship the branch-capable build (already covered by [`04-distribution.md`](04-distribution.md)); PIE/PECL metadata is unchanged because the extension is still `pcov`. | The capability probe is a runtime property of the built `.so`, so no packaging metadata change is required. |

## Phased migration plan

1. **Phase 1 — ship the compat shim now (done).** Route (c): `pcov.mode=branch`
   exposes the Xdebug-compat surface so unmodified `phpunit --path-coverage`
   works today. Validated in [`03-results.md`](03-results.md). Line mode
   unchanged. This is the current release.

2. **Phase 2 — land the Selector probe upstream.** Add `\pcov\capabilities()` /
   `\pcov\collect_path_coverage()` to the extension as honest native API. Propose
   the `PcovBranchDriver` + `Selector` clause + `Runtime` accessor to
   php-code-coverage (route (a)). Both surfaces coexist: if the native driver is
   available it is preferred; otherwise the shim still serves `--path-coverage`.
   Line mode still unchanged.

3. **Phase 3 — optional leaner API.** Once the native driver is upstream, add the
   capability-negotiated compact shape / aggregated collection from (b) as a
   pure marshalling optimisation. Consumers that do not negotiate it keep the
   route-(a) baseline shape.

4. **Phase 4 — deprecate the shim.** After php-code-coverage releases with the
   native pcov branch/path driver and it has propagated to the PHP versions this
   build targets, mark the Xdebug-compat surface deprecated (still opt-in, still
   off by default), then remove it in a later major. pcov stops presenting as
   Xdebug entirely. **Line mode is unaffected at every phase.**

## See also

- [`01-contract.md`](01-contract.md) — the driver contract and `Selector` logic
  this design patches.
- [`02-integration-decision.md`](02-integration-decision.md) — route (a) vs (b)
  vs (c); this document is the expansion of the route-(a) proposal.
- [`03-results.md`](03-results.md) — the validated performance numbers cited in
  the perf rationale.
- [`05-tooling-compatibility.md`](05-tooling-compatibility.md) — how tools behave
  under the *current* (route (c)) build.
- [`04-distribution.md`](04-distribution.md) — distribution/adoption routes
  (setup-php, PIE, PECL, Docker, k8s).
