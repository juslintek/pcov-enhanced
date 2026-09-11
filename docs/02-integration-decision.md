# Step 2 — Integration Route Decision

**Decision: primary route (c) — expose xdebug-compatible coverage functions + `XDEBUG_CC_*`
constants from the pcov extension, guarded so they are inert unless explicitly opted in;
and separately prepare route (a) as the "clean" upstream follow-up.**

This is written before any C is added, so the choice is not relitigated mid-implementation.

## The constraint that forces the choice

Success criterion #1 is literally: *`phpunit --path-coverage` runs under the modified pcov
and reports non-zero Branches and Paths.* From `01-contract.md` §2, `phpunit --path-coverage`
resolves its driver through `Driver\Selector::select()`, which:

- gives pcov work **only** at `Granularity::Line`, and
- for any branch/path granularity uses `XdebugDriver`, which first runs
  `ensureXdebugIsAvailable()` (`extension_loaded('xdebug')`, version `>= 3.1`,
  `'coverage'` in `xdebug_info('mode')`) and then calls the free functions
  `xdebug_start_code_coverage()`, `xdebug_get_code_coverage()`, `xdebug_stop_code_coverage()`,
  `xdebug_set_filter()`, `xdebug_info()`.

So with **unmodified** PHPUnit/php-code-coverage, the only way branch+path data reaches the
report is for something to satisfy those checks and answer those calls. pcov collecting
branches internally is invisible to that path.

## Route-by-route

### (a) Upstream a driver change to php-code-coverage — *prepare, don't block on*
Add a pcov path for branch/path granularity in `Selector`, guarded by a capability probe
(e.g. a new `\pcov\capabilities()` bit or `function_exists('pcov\\collect_path_coverage')`).
- **Pro:** cleanest; no impersonation; pcov stays honestly pcov.
- **Con:** needs Sebastian Bergmann's agreement and a release; not landable within this
  track's control. Fails the "works *today* with unmodified tools" test on its own.
- **Plan:** implement the extension so this is a small, reviewable follow-up (a native
  `pcov\collect()` that returns the xdebug-shaped array for `PCOV_MODE_BRANCH`), and draft
  the `Selector` patch as a proposal. This is the long-term home for the feature.

### (b) Ship a driver class in the consuming project — *rejected as the primary*
Construct `CodeCoverage` with a bespoke `PcovBranchDriver` in loyalty-hub's tooling.
- **Pro:** no external agreement needed; fully honest.
- **Con:** does **not** work through `phpunit --path-coverage` (PHPUnit builds its own
  `Selector`-chosen driver). Directly fails success criterion #1. Useful only for bespoke
  runners, which is not what CI uses.

### (c) Expose xdebug-compatible functions from the extension — *chosen as primary*
Have pcov (in an opt-in mode) register `XDEBUG_CC_*` constants and `xdebug_*` coverage
functions returning the exact shape in `01-contract.md` §1a, and answer
`xdebug_info('mode')`/`phpversion('xdebug')` well enough to pass `ensureXdebugIsAvailable()`.
- **Pro:** the *only* route that makes `phpunit --path-coverage` work with unmodified
  PHPUnit and php-code-coverage — exactly the success criterion. No third-party sign-off.
- **Con:** it means presenting as Xdebug. Upstreams (both pcov and Xdebug) would reject
  shipping this on-by-default. Also collides with Track B, which chose (c) too.
- **How we de-risk the cons:**
  1. **Opt-in only.** The compat surface is dormant unless `pcov.mode=branch` (or env
     `PCOV_MODE=branch`). Default pcov behavior — line coverage, no xdebug symbols — is
     byte-for-byte unchanged, so nothing regresses and it will never *accidentally* claim
     to be Xdebug. Verified as a hard requirement against krakjoe's performance invariant.
  2. **Refuse to co-exist with real Xdebug.** If `extension_loaded('xdebug')` is already
     true, the shim does not register (avoids symbol clash / double coverage). Real Xdebug
     wins; pcov-compat is a stand-in only when Xdebug is absent.
  3. **Honest version string.** `phpversion('xdebug')` reports a sentinel like
     `3.99.0-pcov` (>= 3.1 so the check passes, visibly not a real Xdebug build) and
     `\pcov\...` remains the native API. We satisfy the *interface*, we do not counterfeit
     a specific Xdebug release.

## Coordination with Track B (required)

The brief states Track B also chose (c); "doing it twice is waste." I could not find Track
B's artifacts in this environment (searched the filesystem — nothing), so I cannot import
its shim directly. To make the two efforts converge rather than fork:

- The xdebug-compat surface is isolated in its own translation unit (`pcov_xdebug_compat.c`
  / `.h`) with a single entry point that fills the return `zval` from pcov's internal
  branch model. Whichever track's shim is kept, the other is a drop-in.
- The **data producer** (CFG → branch/path model → xdebug-shaped array) lives in pcov and
  is route-independent, so it feeds route (a) or (c) unchanged. This is the part worth not
  duplicating; the thin function-registration shim is cheap either way.
- **Action item for the human:** reconcile with Track B before shipping (c) publicly — pick
  one shim, keep one producer. If Track B already exposes the functions, this track should
  contribute only the producer + route-(a) proposal.

## What gets built (consequence of the decision)

1. **Producer (route-independent):** extend pcov's existing `zend_build_cfg`-based analysis
   to also record branch edges and enumerate paths (caps copied from xdebug), and a builder
   that emits the `{lines, functions:{branches,paths}}` array. Opt-in via `pcov.mode=branch`.
2. **Native API:** `\pcov\collect()` returns that richer array when in branch mode (feeds
   route (a) and bespoke runners).
3. **Compat shim (route c), opt-in + guarded:** register `XDEBUG_CC_*` and `xdebug_*`
   coverage functions delegating to the producer, so unmodified `phpunit --path-coverage`
   works today.
4. **Follow-up proposal (route a):** a `Selector` patch + capability probe for upstream.
