# Step 1 — The Contract (verified by reading source)

Sources read (all cloned into `/projects/sandbox/src`):
- php-code-coverage `14.4.x-dev` (`8df4080`)
- pcov `1.0.13-dev` (`e8b16fa`)
- xdebug tag `3.6.0alpha1`

## 1. What php-code-coverage requires of a branch-capable driver

### 1a. The `xdebug_get_code_coverage()` shape (the hard contract)

Defined by the phpstan types in
`php-code-coverage/src/Data/RawCodeCoverageData.php` and consumed by
`processXdebugPathAndBranchCoverage()`. When `XDEBUG_CC_BRANCH_CHECK` is on,
each file entry is an **associative** array (not the bare line map):

```
[ "/abs/file.php" => [
    "lines"     => [ <lineNo:int> => <status:int> ],      // -2 dead, -1 not-run, >=1 hit
    "functions" => [ "<funcKey>" => [
        "branches" => [ <branchId:int> => [
            "op_start"   => int,   // opcode index where branch starts (branchId == op_start)
            "op_end"     => int,   // opcode index where branch ends
            "line_start" => int,
            "line_end"   => int,
            "hit"        => int,   // 0 or 1 (xdebug reports 0/1, not a count)
            "out"        => [ <j:int> => <targetBranchId:int> ],     // sparse; 0-targets omitted
            "out_hit"    => [ <j:int> => <0|1> ],                    // parallel to "out"
        ] ],
        "paths" => [ <pathIdx:int> => [
            "path" => [ <int>, <int>, ... ],   // sequence of branch ids
            "hit"  => int,                     // 0 or 1
        ] ],
    ] ],
] ]
```

`funcKey` naming (from `xdebug_build_fname_from_oparray` + `xdebug_func_format`):
- `"{main}"` — file-level (top-level) code, `op_array->function_name == NULL`
- `"Class->method"` — any method whose `op_array->scope` is set (instance *or* static; xdebug always uses `->`)
- `"funcName"` — a plain function
- closures are wrapped `"{closure:/file.php:NN}"`; trait methods get a `{trait-method:...}`
  suffix that php-code-coverage strips in `processXdebugPathAndBranchCoverage()`.

Line-only shape (no branch check) is the bare `[ file => [ lineNo => status ] ]` that
pcov already produces.

### 1b. Invariants the consumer relies on / tolerates
- `op_start`/`op_end` are opcode indexes; consumer treats them as identity+ordering only.
- The branch id **is** its `op_start` (xdebug keys `branches` by opcode index `i`).
- `out`/`out_hit` may be empty — reports degrade gracefully.
- Loop back-edges can have `line_start > line_end`; the consumer normalizes, but we
  should still emit them the way xdebug does so numbers match.
- `hit` for branch/path is 0/1 in xdebug (not a traversal count). We match that.

## 2. How the driver is selected — the part that kills naive attempts

`php-code-coverage/src/Driver/Selector.php::select()`:

```php
if ($granularity === Granularity::Line && $runtime->hasPCOV()) return new PcovDriver($filter);
if ($runtime->hasXdebug())                                     return new XdebugDriver(...);
throw new NoSupportedDriverAvailableException($granularity);
```

- `PcovDriver` is selected **only** for `Granularity::Line`. Any branch/path granularity
  skips pcov entirely and goes to `XdebugDriver`.
- `XdebugDriver::__construct` calls `ensureXdebugIsAvailable()`, which requires
  `extension_loaded('xdebug')`, version `>= 3.1`, and `'coverage'` in `xdebug_info('mode')`.
- `XdebugDriver::start()` sends `XDEBUG_CC_UNUSED | XDEBUG_CC_DEAD_CODE | XDEBUG_CC_BRANCH_CHECK`
  and `stop()` calls `xdebug_get_code_coverage()` then `xdebug_stop_code_coverage()`.

**Consequence:** teaching pcov to *collect* branches is necessary but NOT sufficient to
reach `phpunit --path-coverage`. The CLI path only ever asks pcov for `Line`. To be used
for branch/path through unmodified PHPUnit, either (a) `Selector` must change, or (c) the
extension must satisfy the `XdebugDriver` availability checks and expose the `xdebug_*`
coverage functions + `XDEBUG_CC_*` constants.

## 3. How pcov collects today (and krakjoe's stance)

`pcov/pcov.c`:
- **Compile hook** `php_pcov_compile_file` stashes each wanted `zend_op_array` in
  `PCG(files)` (refcounted) so the report step can re-derive executable lines.
- **Runtime** overrides `zend_execute_ex` with `php_pcov_execute_ex` →
  `php_pcov_trace` runs **per opcode**: if enabled and the file is wanted and the opcode is
  not "ignored" and the `(file,line)` pair is not already seen, it appends a
  `php_coverage_t {file, line, next}` node to a linked list. This is a *line-hit log* — no
  edges, no branch ids, no opcode index.
- **Report** `\pcov\collect()` → `php_pcov_discover_file` → `php_pcov_discover_code`:
  **pcov already builds a Zend CFG** via `zend_build_cfg(arena, ops, PHP_PCOV_CFG, &cfg)`
  (the Optimizer's CFG). It walks `cfg.blocks` only to enumerate *executable line numbers*
  (initialized to -1), then `php_pcov_report` flips lines present in the hit-log to 1.
  The block edge structure is discarded.

**krakjoe's documented reasoning (pcov `README.md`):**
- L122: "PCOV uses the very well proven control flow graph from Optimizer" — he already
  does CFG-based analysis for executable-line detection.
- L145: Xdebug "has path coverage and PCOV does not, **although path coverage is not yet
  implemented (and probably won't be) by CodeCoverage**."
- L150: pcov's whole value is low overhead: "PCOV is less than 1000 lines of code (not
  including CFG) and doesn't have anything like the overhead of a debugger."

So what he declined was **not** the control-flow analysis (he relies on it already); it was
building path coverage that, at the time, `php-code-coverage` could not consume. That
consumer support now exists (`RawCodeCoverageData::fromXdebugWith*Coverage`,
`Granularity::LineBranchAndPath`). The objection is dated, not principled — but his
*performance* invariant (don't turn pcov into a debugger) is the real constraint to honor.

## 4. Xdebug reference algorithm (what we replicate for semantics)

`xdebug/src/coverage/code_coverage.c` + `branch_info.c`:
- **Branch cutting** `xdebug_find_jumps(opa, pos, &n, jumps)` maps an opcode to successor
  opcode indexes. Handled opcodes: `ZEND_JMP` (1 out), `ZEND_JMPZ/JMPNZ/JMPZ_EX/JMPNZ_EX`
  (fallthrough+target), `ZEND_FE_FETCH_R/RW` (next + `extended_value/sizeof(op)`),
  `ZEND_FE_RESET_R/RW`, `ZEND_CATCH` (next + op2 unless `ZEND_LAST_CATCH`), `ZEND_GOTO`,
  `ZEND_FAST_CALL`, `ZEND_FAST_RET` (EXIT), `ZEND_JMP_FRAMELESS` (PHP 8.4+: next + op2),
  `ZEND_MATCH/SWITCH_LONG/SWITCH_STRING` (one out per jumptable entry + default [+ next
  except MATCH]), and terminators `ZEND_RETURN/GENERATOR_RETURN/THROW/MATCH_ERROR` (and
  `exit` via INIT_FCALL scan) → EXIT.
- **Basic-block build** `xdebug_analysis_branch` recursively splits at jump targets,
  recording `starts`, `ends`, per-branch `outs[]`, `start_lineno`; `xdebug_branch_post_process`
  coalesces runs into branches and fills `end_op`/`end_lineno`. Entry points: opcode 0 and
  each `ZEND_CATCH` (chained catches pruned by `only_leave_first_catch`).
- **Path enumeration** `xdebug_branch_find_path`: DFS from each entry point, appending
  branch ids, refusing to re-traverse an edge already on the current path
  (`xdebug_path_exists`) — a self-avoiding-edge walk, which is how it terminates on loops.
- **Caps** (`branch_info.h`): `XDEBUG_MAX_PATHS 4096`, `XDEBUG_BRANCH_MAX_OUTS 64`.
  `xdebug_branch_find_path` bails as soon as `paths_count >= XDEBUG_MAX_PATHS`. We copy
  both constants and the bail rather than inventing a cap.
- **Output** `add_branches` keys `branches` by opcode index `i`; `add_paths` is a plain
  list; `add_file` wraps in `{lines, functions}` only when branch check is active.
