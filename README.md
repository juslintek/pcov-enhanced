PCOV
====

[![CI](https://github.com/juslintek/pcov-enhanced/actions/workflows/ci.yml/badge.svg)](https://github.com/juslintek/pcov-enhanced/actions/workflows/ci.yml)

A self contained [CodeCoverage](https://github.com/sebastianbergmann/php-code-coverage) compatible driver for PHP

> **This is an alternative pcov build that adds branch and path coverage**
> (like Xdebug, but far faster). It is a drop-in replacement for pcov: line
> coverage behaves exactly as upstream, and `pcov.mode=branch` additionally
> provides branch/path coverage consumable through `phpunit --path-coverage`.
> See the [branch/path coverage](#branch-and-path-coverage-pcovmodebranch)
> section below.

## Continuous integration & reports

The [CI workflow](.github/workflows/ci.yml) builds the extension across a PHP
8.2–8.4 matrix (opcache on/off), runs the `.phpt` suite in both line and branch
mode, runs the suite under Valgrind, and generates **branch** and **path**
coverage reports (Markdown tables, `coverage.json`, a PHPUnit HTML report and a
Cobertura XML) which are published to the job summary and uploaded as the
`coverage-reports` artifact.

Requirements and Installation
=============================

See [INSTALL.md](INSTALL.md) for build-from-source, PECL tarball, and Composer
installation, and [docs/04-distribution.md](docs/04-distribution.md) for the
full distribution/registry guide. This build installs as the `pcov` extension
and replaces upstream pcov (only one may be installed at a time).


API
===

```php
/**
 * Shall start recording coverage information
 */
function \pcov\start() : void;

/**
 * Shall stop recording coverage information
 */
function \pcov\stop() : void;

/**
 * Shall collect coverage information
 *
 * @param integer $type define witch type of information should be collected
 *		 \pcov\all        shall collect coverage information for all files
 *		 \pcov\inclusive  shall collect coverage information for the specified files
 *		 \pcov\exclusive  shall collect coverage information for all but the specified files
 * @param array $filter path of files (realpath) that should be filtered
 *
 * @return array
 */
function \pcov\collect(int $type = \pcov\all, array $filter = []) : array;

/**
 * Shall clear stored information
 *
 * @param bool $files set true to clear file tables
 *
 * Note: clearing the file tables may have surprising consequences
 */
function \pcov\clear(bool $files = false) : void;

/**
 * Shall return list of files waiting to be collected
 */
function \pcov\waiting() : array;

/**
 * Shall return the current size of the trace and cfg arena
 */
function \pcov\memory() : int;
```

Configuration
=============

PCOV is configured using PHP.ini:

| Option                 | Default            | Changeable     | Description                                           |
|:-----------------------|:-------------------|:--------------:|:------------------------------------------------------|
| `pcov.enabled`         | 1                  | SYSTEM         | enable or disable zend hooks for pcov                 |
| `pcov.directory`       | auto               | SYSTEM,PERDIR  | restrict collection to files under this path          |
| `pcov.exclude`         | unused             | SYSTEM,PERDIR  | exclude files under pcov.directory matching this PCRE |
| `pcov.initial.memory`  | 65536              | SYSTEM,PERDIR  | shall set initial size of arena                       |
| `pcov.initial.files`   | 64                 | SYSTEM,PERDIR  | shall set initial size of tables                      |
| `pcov.mode`            | line               | SYSTEM         | `line` (default) or `branch` for branch/path coverage; startup-only (must be set before startup, e.g. `-d`/php.ini or `PCOV_MODE`, not `.user.ini`) |

Branch and path coverage (`pcov.mode=branch`)
----------------------------------------------

By default PCOV collects line coverage only, exactly as before. Setting
`pcov.mode=branch` (or the environment variable `PCOV_MODE=branch`) additionally
collects **branch and path coverage** in the same shape Xdebug produces, so it
can be consumed by `phpunit --path-coverage`.

`\pcov\collect()` then returns, per file, `['lines' => ..., 'functions' => ...]`
where each function carries `branches` and `paths` compatible with
`SebastianBergmann\CodeCoverage`.

To make **unmodified** PHPUnit / php-code-coverage drive PCOV for path coverage,
branch mode also exposes an opt-in Xdebug-compatible surface (the `XDEBUG_CC_*`
constants and `xdebug_*` coverage functions). This surface:

  * is **only** active when `pcov.mode=branch`; line mode is unchanged,
  * **never** registers when the real Xdebug extension is loaded (Xdebug wins;
    PCOV downgrades to line mode to avoid double instrumentation),
  * reports `phpversion('xdebug')` as the sentinel `3.99.0-pcov`, so it is
    clearly identifiable and not mistaken for a real Xdebug build.

Branch/path coverage costs roughly `1.5`–`2x` PCOV line coverage and remains
dramatically cheaper than Xdebug path coverage. See `docs/` for the full
contract, the integration-route decision, and validation results.

Notes
-----

The recommended defaults for production should be:

  * `pcov.enabled = 0`

The recommended defaults for development should be:

  * `pcov.enabled = 1`
  * `pcov.directory = /path/to/your/source/directory`

When `pcov.directory` is left unset, PCOV will attempt to find `src`, `lib` or, `app` in the current
working directory, in that order; If none are found the current directory will be used, which may waste resources storing
coverage information for the test suite.

If `pcov.directory` contains test code, it's recommended to set `pcov.exclude` to avoid wasting resources.

To avoid unnecessary allocation of additional arenas for traces and control flow graphs, `pcov.initial.memory` should be set according to the memory required by the test suite, which may be discovered with `\pcov\memory()`.

To avoid reallocation of tables, `pcov.initial.files` should be set to a number higher than the number of files that will be loaded during testing, inclusive of test files.

*Note that arenas are allocated in chunks: If the chunk size is set to 65536 and pcov require 65537 bytes, the system will allocate two chunks, each 65536 bytes. When setting arena space therefore, be generous in your estimates.*

Interoperability
================

When PCOV is enabled by configuration `pcov.enabled=1`:

  * interoperability with Xdebug is not possible
  * interoperability with phpdbg is not possible
  * interoperability with Blackfire profiler is not possible

At an internals level, the executor function is overriden by pcov, so any extension or SAPI which does the same will be broken.

When PCOV is disabled by configuration `pcov.enabled=0`:

  * PCOV is zero cost - code runs at full speed
  * Xdebug may be loaded
  * phpdbg may be executed
  * Blackfire probe may be loaded

At an internals level, the executor function is untouched, and pcov allocates no memory.

Differences in Reporting
========================

There are subtle differences between Xdebug and PCOV in reporting: Both Xdebug and PCOV perform branch analysis in order to detect executable code. Xdebug has custom written (very mature, proven) analysis, while PCOV uses the very well proven control flow graph from Optimizer. They generate comparably accurate reports, while phpdbg uses less robust detection of executable code and generates reports with known defects. One such defect in phpdbg is this:

```php
/* 2 */ function foo($bar) {
/* 3 */ 	if ($bar) {
/* 4 */			return true;
/* 5 */		}
/* 6 */	}
```

phpdbg will detect that this function is 100% covered when the first control path is taken, `if ($bar)`, because it cannot correctly detect which implicit return paths inserted by Zend at compile time are executable, and so chooses to ignore them all. While this may seem like a trivial difference to some, it means that the reports generated by phpdbg are not completely trustworthy.

While the accuracy of Xdebug and PCOV are comparable, the reports they generate are not precisely the same, one such example is the `switch` construct:

```php
/* 2 */ switch ($condition) {
/* 3 */		case 1:
/* 4 */			return "PHP rox!";
/* 5 */	}
```

From PHP 7.2.15 and PCOV 1.0, PCOV will detect the executability of the cases inside the switch body correctly, but will not detect line 2 (with the `switch` statement) as executable because Zend didn't output an executable opcode on that line. Xdebug's custom analysis doesn't use the same method and so will show an extra executable line on 2. Pre 7.2.15 and PCOV 1.0, the coverage of some switches is questionable as a result of the way Zend constructs the opcodes in the body of the switch - it may not execute them depending on a jump table optimization.

While Xdebug and PCOV both do the same kind of analysis of code, Xdebug is currently able to do more with that information than just generate accurate line coverage reports, it has path coverage and PCOV does not, although path coverage is not yet implemented (and probably won't be) by CodeCoverage.

Differences in Performance
==========================

The differences in performance of Xdebug and PCOV are not slight. Xdebug is first and foremost a debugging extension, and when you load it, you incur the overhead of a debugger even when it's disabled. PCOV is less than 1000 lines of code (not including CFG) and doesn't have anything like the overhead of a debugger.
