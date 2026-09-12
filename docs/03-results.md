# Step 3 — Results & Validation

Branch and path coverage added to pcov, consumable through unmodified
`phpunit --path-coverage`, at a small fraction of Xdebug's cost.

## What was built

| File | Role |
|------|------|
| `pcov_branch.c` / `.h` | Route-independent producer: static branch/path analysis (a faithful port of Xdebug 3.6's `code_coverage.c` + `branch_info.c`) and the array builder that emits the exact `{branches, paths}` shape php-code-coverage consumes. Path enumeration caps copied from Xdebug (`PCOV_MAX_PATHS 4096`, `PCOV_BRANCH_MAX_OUTS 64`). |
| `pcov_xdebug_compat.c` / `.h` | Integration route (c): registers `XDEBUG_CC_*` constants and `xdebug_*` coverage functions + a stand-in module named `xdebug` so the unmodified `XdebugDriver` accepts pcov. Opt-in, guarded. |
| `pcov.c` (modified) | `pcov.mode` INI (`line` default / `branch`); per-op_array analysis cache; runtime per-invocation path recording (`pcov_frame_t` stack); branch-mode `\pcov\collect()` emitting `{lines, functions}`; guardrails. |

## Design summary (see 01-contract.md, 02-integration-decision.md)

- **Static analysis** ports Xdebug's opcode-indexed branch model so branch ids
  and topology are the same shape Xdebug produces. pcov already built a Zend
  CFG for line detection; this adds the edge/path modelling Xdebug does.
- **Runtime hit tracking is deferred and cheap.** The per-opcode hook records,
  per live frame, only the sequence of *branch-start* opcodes entered. Branch
  hits, out-edge hits and path hits are computed at `collect()` time. A path is
  hit only when the recorded per-invocation sequence exactly equals a
  statically enumerated path — the same rule as Xdebug's
  `mark_end_of_function_reached`.
- **Route (c) is opt-in and guarded.** The xdebug-compat surface is dormant
  unless `pcov.mode=branch` (or `PCOV_MODE=branch`), never registers when a
  real Xdebug is present (`zend_get_extension("Xdebug")` / module check), and
  reports `phpversion('xdebug') == "3.99.0-pcov"` — a visible sentinel, not a
  counterfeit release.

## Success criteria — all met

Validated on **PHP 8.4.24** (host) and **PHP 8.5.10** (`php:8.5-cli-alpine`,
API 20250925 — the brief's target family; the built `.so` also loads in
`wodby/php:8.5`, same API). Reference driver: Xdebug **3.6.0alpha1** built from
its tag.

### 1. `phpunit --path-coverage` reports non-zero Branches and Paths
Unmodified PHPUnit 11.5.56 + php-code-coverage 11.0, driver selected by pcov's
shim:
```
Runtime: PHP 8.5.10 with Xdebug 3.99.0-pcov
Paths:    60.00% (6/10)
Branches: 100.00% (18/18)
Lines:    100.00% (13/13)
```
(Identical on PHP 8.4.24.)

### 2. Numbers match Xdebug on the required corpus
Corpus covers `if`/`else`, `foreach`, `match`, `try`/`catch`/`finally`,
generators, first-class callables, and a frameless internal call (`in_array`
with **variable** args — verified to emit `FRAMELESS_ICALL_2` / `ZEND_JMP_FRAMELESS`):

| Metric | Xdebug 3.6 | pcov branch |
|--------|-----------:|------------:|
| total branches | 35 | **35** |
| hit branches   | 35 | **35** |
| total paths    | 21 | **21** |
| hit paths      | 10 | **10** |

Raw per-method output for a class under test (`Calc`) is identical to Xdebug
(19 branches / 11 paths, per-method exact). Path explosion caps identically:
`explode_paths` reports 27 branches / **4096** paths under both.

- **Opcode indexes differ** between the two (pcov compiles op_arrays with
  `ZEND_COMPILE_NO_JUMPTABLES`), so absolute branch ids are not identical — but
  php-code-coverage's Branch%/Path% depend only on branch/path counts and
  `hit >= 1`, which match. This is expected and documented.
- **Known benign difference:** for a file consisting only of declarations whose
  top-level `{main}` ran *before* coverage started (e.g. `require`d before
  `start()`), pcov still reports that file's trivial `{main}` (1 branch, hit=0)
  while Xdebug omits it. This only ever *over*-states executable paths by 1 per
  such file; it never affects hit counts and never under-reports coverage. When
  a file's `{main}` executes under coverage, it matches Xdebug exactly
  (b=1/p=1/hit=1).

### 3. Valgrind clean (not "it didn't crash")
`USE_ZEND_ALLOC=0 ZEND_DONT_UNLOAD_MODULES=1 valgrind --leak-check=full`:
- Frameless `in_array` 5-liner (the exact case that broke Xdebug 3.5): **0 errors, 0 bytes lost** — on both 8.4 and 8.5.
- Full corpus: **0 errors, 0 bytes lost**.
- Stress (2^13 path explosion capped at 4096, repeated `collect()`, `clear()`, re-run): **0 errors, 0 bytes lost**.

### 4. Faster than Xdebug (target stated up front)
**Target set before measuring: pcov branch coverage must stay within a small
constant factor of pcov line coverage and be at least an order of magnitude
faster than Xdebug path coverage.** Branch-heavy workload, per-iteration
(PHP 8.4.24):

| Configuration | per iteration | vs baseline |
|---------------|--------------:|------------:|
| no coverage | 0.026 ms | 1× |
| pcov line | 0.35 ms | 13× |
| **pcov branch** | **0.60 ms** | **23×** |
| Xdebug path | 243.8 ms | 9250× |

pcov branch coverage is **~406× faster than Xdebug path coverage** here (and
~1.7× the cost of pcov line coverage). The margin far exceeds the brief's bar
(Xdebug turned a 6m18s suite into ~15m ≈ 2.4×). The microbenchmark is
recursion-heavy and thus flattering to pcov, but the order-of-magnitude
conclusion is robust.

## Guardrails verified
1. **Line mode (default):** no `xdebug` symbols exposed; behavior byte-for-byte
   unchanged from upstream pcov.
2. **Branch mode alone:** shim active; `phpversion('xdebug') == 3.99.0-pcov`.
3. **Branch mode + real Xdebug loaded:** shim declines, pcov downgrades to line
   mode, real Xdebug wins — no crash. (An earlier version double-registered a
   module named `xdebug` and faulted in `zend_post_startup`; fixed by refusing
   when the `Xdebug` Zend extension is present.)

## Environment notes
- Host: PHP 8.4.24 NTS x86_64 (API 20240924). Target: PHP 8.5.10 (API 20250925)
  via `php:8.5-cli-alpine`; the module also loads in `wodby/php:8.5`.
- Build: `phpize && ./configure --enable-pcov CFLAGS="-g -O0" && make`. Compiles
  clean, no warnings, on both versions.

## Coordination / follow-ups
- Route (c) collides with Track B (which also chose (c)); Track B artifacts were
  not present in this environment. The producer (`pcov_branch.*`) is
  route-independent and is the part worth not duplicating; reconcile the thin
  shim with Track B before shipping (c) publicly.
- Route (a) — a `Selector` capability-probe patch upstream — remains the clean
  long-term home and is a small follow-up on top of this producer.
