# Changelog

All notable changes to this alternative pcov distribution are documented here.
This project builds and loads as the `pcov` extension and tracks upstream
[krakjoe/pcov](https://github.com/krakjoe/pcov); entries below describe what
this distribution adds on top of it.

## [Unreleased]

### Documentation
- **Coverage-tooling compatibility matrix.** Added
  `docs/05-tooling-compatibility.md` documenting how each coverage-consuming
  tool works with this drop-in `pcov`/Xdebug-compat build: PHPUnit +
  php-code-coverage (line via `PcovDriver`; branch/path via `--path-coverage`
  through the shim), Infection, Paratest, Codeception, Behat, and the
  Coveralls/Codecov/Scrutinizer uploaders (driver-agnostic Clover/Cobertura
  consumers). States driver selection per tool, line + branch/path support,
  caveats, and the real-Xdebug coexistence rule; cites the `Selector` logic from
  `docs/01-contract.md` and the validated numbers from `docs/03-results.md`.
- **Future native-API design.** Added `docs/06-future-native-api.md`, a
  forward-looking design for the clean route (a): a php-code-coverage `Selector`
  capability probe (`\pcov\capabilities()` / `pcov\collect_path_coverage`) and a
  native `PcovBranchDriver` (no Xdebug impersonation, reusing the existing
  `pcov_branch.c` producer), a leaner native collection API proposal, the
  infrastructure adoption requirements, and a phased migration plan (ship compat
  shim now -> land Selector probe upstream -> deprecate shim). The line-mode
  invariant is kept explicit throughout. Cross-linked both new docs from
  `README.md` and `docs/04-distribution.md`.

### Distribution
- **GitLab CI pipeline.** Added `.gitlab-ci.yml` mirroring the GitHub flow on
  the official `php:8.2`/`8.3`/`8.4` images: `build` -> `test` (both line and
  branch mode) -> `package` (`pecl package-validate`/`pecl package`) ->
  tag-gated `release` (`rules: if $CI_COMMIT_TAG`) that publishes a GitLab
  Release via `release-cli`.
- **Prebuilt-binary release assets.** On tag pushes, CI now attaches a
  per-PHP-version `modules/pcov.so` named to encode version + PHP version + OS +
  arch + thread-safety (e.g. `pcov-pcov-enhanced-1.1.0-php8.3-linux-x86_64-nts.so`)
  plus a `SHA256SUMS.txt`, so consumers (e.g. `setup-php`) can install without a
  toolchain. GitHub: new `prebuilt-binaries` + `checksums` jobs in
  `.github/workflows/ci.yml` (reusing the SHA-pinned `softprops/action-gh-release`,
  `contents: write` scoped to release/packaging jobs only). Documented the full
  release-automation flow, the manual PECL/Packagist publish commands, and a
  copy-pasteable `setup-php` consumption recipe in `docs/04-distribution.md`.
- **PIE support documented.** Added a PIE (PHP Installer for Extensions,
  `php/pie`) install route (`pie install juslintek/pcov-enhanced`) to
  `docs/04-distribution.md` and `INSTALL.md`. PIE reuses the existing
  `composer.json` `php-ext` metadata and builds from source; `config.m4` is at
  the repo root so no `php-ext.build-path` override is needed.
- **package.xml date handling.** Set `<date>` to the current UTC date and
  documented that it must be bumped per release (removes the PECL "Release Date
  is not today" validator warning at package time). Added an XML comment noting
  the "providesextension name differs from package name" warning is intentional
  (we publish as `pcov_enhanced` while providing the drop-in `pcov` extension).
- **Repo-rename clarified.** Documented that the GitHub repository stays named
  `pcov-enhanced` and that package identity does not depend on the repo name
  (it comes from `package.xml` / `composer.json`); a manual rename by a
  maintainer is optional.
- Added a `support.docs` link to `composer.json`.

## [1.1.0] - 2026-09-10

### Added
- **Branch and path coverage** via a new `pcov.mode` INI setting
  (`line` default — unchanged behaviour; `branch` enables branch/path).
  In branch mode, `\pcov\collect()` returns the `{lines, functions}` shape
  (with per-function `branches` and `paths`) that
  `phpunit/php-code-coverage` consumes.
- **Xdebug-compatible surface** (opt-in, only under `pcov.mode=branch`) so
  that unmodified PHPUnit selects this extension for `--path-coverage`. It
  registers `XDEBUG_CC_*` / `XDEBUG_PATH_*` / `XDEBUG_FILTER_*` constants and
  `xdebug_start_code_coverage()`, `xdebug_stop_code_coverage($cleanup)`,
  `xdebug_get_code_coverage()`, `xdebug_set_filter()`, `xdebug_info()`.
  - Dormant unless branch mode is requested.
  - Never registers when the real Xdebug extension is present (Xdebug wins;
    pcov downgrades to line mode to avoid double instrumentation).
  - Reports `phpversion('xdebug')` as the sentinel `3.99.0-pcov`.
- `xdebug_set_filter()` include / exclude / `XDEBUG_FILTER_NONE` path-prefix
  filtering, applied both at trace time and at collection time.
- Tests: `014` (branch/path shape), `015` (set_filter exclude), `016`
  (`xdebug_stop_code_coverage($cleanup)` reset vs preserve).

### Fixed (review round 2)
- Corrected the xdebug-compat filter constants to Xdebug's real values
  (`XDEBUG_FILTER_CODE_COVERAGE=256`, `XDEBUG_PATH_INCLUDE=1`,
  `XDEBUG_PATH_EXCLUDE=2`, `XDEBUG_FILTER_NONE=0`) and the include/exclude/none
  list-type mapping, so php-code-coverage's `XdebugDriver` filter calls are
  honored instead of silently ignored.
- Bounds-check every opcode index derived from a jump operand before use in the
  branch analysis (prevents an out-of-bounds read on malformed/edge-case CFGs).
- Normalize a missing `$filter` argument to an empty array in `\pcov\collect()`
  so include/exclude paths never dereference NULL.
- Cache the CFG analysis for never-executed functions (with empty hit sets)
  instead of rebuilding and re-enumerating paths on every `collect()`.
- Tie each branch cache entry to the op_array's identity (filename + line +
  size); a reused `opcodes` address is detected and the stale entry rebuilt.
- Report exit-edge `out_hit` from the recorded runtime exit transition, so a
  `ZEND_LAST_CATCH` whose catch matched no longer reports its exit edge taken.
- Line-shape `.phpt` guards now honor `PCOV_MODE` precedence and treat `path`
  as branch mode; the cleanup test also asserts coverage is preserved after
  `xdebug_stop_code_coverage(false)`.

### Fixed (review round 4)
- Detach/finalize any live runtime frame whose cached analysis is being evicted
  (stale `opcodes`-address collision) before freeing it, preventing a
  use-after-free through `pcov_frame_t.cache`.
- Bounds-check path-enumeration targets so a jump-derived index can never read
  `info->branches` out of range.
- Pin the third-party release action to a full commit SHA and scope
  `contents: write` to the packaging job only (supply-chain hardening).

### Notes
- Branch analysis is a faithful port of Xdebug 3.6's control-flow and
  path-enumeration logic; path enumeration is capped identically (4096 paths).
- Branch/path coverage costs roughly 1.5–2× pcov line coverage and remains
  dramatically cheaper than Xdebug path coverage.
- Validated on PHP 8.2–8.5; Valgrind-clean including the frameless internal
  call case that historically broke Xdebug 3.5.

## Upstream base

Based on pcov 1.0.x (`krakjoe/pcov`). See that project for the history of the
line-coverage driver this distribution extends.
