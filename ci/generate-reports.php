<?php declare(strict_types=1);
/*
 * CI report generator for pcov branch/path coverage.
 *
 * Runs the fixture's tested code under pcov branch mode, collects branch and
 * path coverage via the native \pcov\collect() API, and emits:
 *   - build/reports/branch-coverage.md  (per-function branch table)
 *   - build/reports/path-coverage.md    (per-function path table)
 *   - build/reports/coverage.json       (machine-readable totals)
 *   - build/reports/summary.md          (combined summary for the job step)
 *
 * Requires: extension=pcov, pcov.enabled=1, pcov.mode=branch, and
 * pcov.directory pointing at the fixture src/.
 *
 * Usage: php ci/generate-reports.php <fixture-dir> <output-dir>
 */

if (!extension_loaded('pcov')) {
    fwrite(STDERR, "pcov not loaded\n");
    exit(2);
}
if (!function_exists('pcov\\collect')) {
    fwrite(STDERR, "\\pcov\\collect() unavailable\n");
    exit(2);
}

$fixtureDir = rtrim($argv[1] ?? (__DIR__ . '/report-project'), '/');
$outDir     = rtrim($argv[2] ?? (dirname(__DIR__) . '/build/reports'), '/');
$srcDir     = $fixtureDir . '/src';

require $fixtureDir . '/vendor/autoload.php';

// --- exercise the code under coverage -----------------------------------
\pcov\start();
$s = new \Sample\Sample();
$s->classify(-3); $s->classify(0); $s->classify(7);
$s->sumEven([1, 2, 3, 4]);
$s->label(-1); $s->label(0); $s->label(3);
$s->safeDivide(4, 2); $s->safeDivide(4, 0);
$s->lengths(['a', 'bb', 'ccc']);
\pcov\stop();

$data = \pcov\collect(\pcov\inclusive, [realpath($srcDir . '/Sample.php')]);

@mkdir($outDir, 0777, true);

// --- aggregate ------------------------------------------------------------
$totBranches = $hitBranches = $totPaths = $hitPaths = 0;
$branchRows = [];
$pathRows = [];

foreach ($data as $file => $fileData) {
    $rel = str_replace(dirname($fixtureDir) . '/', '', $file);
    foreach (($fileData['functions'] ?? []) as $fn => $fd) {
        $b = $fd['branches'] ?? [];
        $p = $fd['paths'] ?? [];
        $bh = 0; foreach ($b as $x) { if (($x['hit'] ?? 0) >= 1) { $bh++; } }
        $ph = 0; foreach ($p as $x) { if (($x['hit'] ?? 0) >= 1) { $ph++; } }

        $totBranches += count($b); $hitBranches += $bh;
        $totPaths += count($p);    $hitPaths += $ph;

        $bp = count($b) ? round($bh / count($b) * 100, 1) : 100.0;
        $pp = count($p) ? round($ph / count($p) * 100, 1) : 100.0;
        $branchRows[] = sprintf('| `%s` | `%s` | %d | %d | %.1f%% |', $rel, $fn, count($b), $bh, $bp);
        $pathRows[]   = sprintf('| `%s` | `%s` | %d | %d | %.1f%% |', $rel, $fn, count($p), $ph, $pp);
    }
}

$branchPct = $totBranches ? round($hitBranches / $totBranches * 100, 1) : 100.0;
$pathPct   = $totPaths ? round($hitPaths / $totPaths * 100, 1) : 100.0;

// --- write reports --------------------------------------------------------
$branchMd  = "# Branch Coverage Report\n\n";
$branchMd .= sprintf("**Total: %d/%d branches (%.1f%%)**\n\n", $hitBranches, $totBranches, $branchPct);
$branchMd .= "| File | Function | Branches | Hit | Coverage |\n|------|----------|---------:|----:|---------:|\n";
$branchMd .= implode("\n", $branchRows) . "\n";
file_put_contents($outDir . '/branch-coverage.md', $branchMd);

$pathMd  = "# Path Coverage Report\n\n";
$pathMd .= sprintf("**Total: %d/%d paths (%.1f%%)**\n\n", $hitPaths, $totPaths, $pathPct);
$pathMd .= "| File | Function | Paths | Hit | Coverage |\n|------|----------|------:|----:|---------:|\n";
$pathMd .= implode("\n", $pathRows) . "\n";
file_put_contents($outDir . '/path-coverage.md', $pathMd);

$json = [
    'driver'   => 'pcov (branch mode) ' . (phpversion('pcov') ?: 'unknown'),
    'php'      => PHP_VERSION,
    'branches' => ['total' => $totBranches, 'hit' => $hitBranches, 'percent' => $branchPct],
    'paths'    => ['total' => $totPaths, 'hit' => $hitPaths, 'percent' => $pathPct],
];
file_put_contents($outDir . '/coverage.json', json_encode($json, JSON_PRETTY_PRINT) . "\n");

$summary  = "## pcov branch & path coverage\n\n";
$summary .= sprintf("- **PHP:** %s\n- **Driver:** %s\n", PHP_VERSION, $json['driver']);
$summary .= sprintf("- **Branches:** %d/%d (%.1f%%)\n", $hitBranches, $totBranches, $branchPct);
$summary .= sprintf("- **Paths:** %d/%d (%.1f%%)\n\n", $hitPaths, $totPaths, $pathPct);
$summary .= $branchMd . "\n" . $pathMd;
file_put_contents($outDir . '/summary.md', $summary);

fwrite(STDOUT, sprintf(
    "branch=%d/%d (%.1f%%) path=%d/%d (%.1f%%)\n",
    $hitBranches, $totBranches, $branchPct, $hitPaths, $totPaths, $pathPct
));

// Non-zero branches AND paths is the health check.
exit(($totBranches > 0 && $totPaths > 0 && $hitBranches > 0 && $hitPaths > 0) ? 0 : 1);
