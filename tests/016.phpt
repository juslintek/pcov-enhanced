--TEST--
branch mode: xdebug_stop_code_coverage($cleanup) resets vs preserves
--SKIPIF--
<?php
if (!extension_loaded("pcov")) print "skip pcov not loaded";
$__m = getenv("PCOV_MODE"); if ($__m === false || $__m === "") { $__m = (string) ini_get("pcov.mode"); }
if (strcasecmp($__m, "branch") !== 0 && strcasecmp($__m, "path") !== 0) print "skip requires branch mode";
if (!function_exists("xdebug_start_code_coverage")) print "skip compat surface inactive (real xdebug present?)";
?>
--INI--
pcov.enabled = 1
pcov.mode = branch
pcov.directory = .
--FILE--
<?php
$dir = __DIR__;
file_put_contents($dir . "/016_lib.php", "<?php function f016(\$n){ if(\$n>0) return 1; return 0; }\n");
require $dir . "/016_lib.php";

function count_hits(array $data): int {
    $n = 0;
    foreach ($data as $fd) {
        foreach (($fd['functions'] ?? []) as $fn) {
            foreach ($fn['branches'] as $b) { if ($b['hit'] >= 1) $n++; }
        }
    }
    return $n;
}

// Run 1: cleanup=true => state discarded afterward.
xdebug_start_code_coverage(XDEBUG_CC_BRANCH_CHECK);
f016(1);
$first = xdebug_get_code_coverage();
xdebug_stop_code_coverage(true);
var_dump(count_hits($first) > 0);          // coverage was collected

// After cleanup, a fresh collection with no execution has no hits.
xdebug_start_code_coverage(XDEBUG_CC_BRANCH_CHECK);
$afterCleanup = xdebug_get_code_coverage();
xdebug_stop_code_coverage(false);
var_dump(count_hits($afterCleanup) === 0); // cleanup discarded prior hits

// Run 2: cleanup=false => state preserved. Execute, stop(false), then a fresh
// collection must still report the previously recorded hits.
xdebug_start_code_coverage(XDEBUG_CC_BRANCH_CHECK);
f016(1);
xdebug_stop_code_coverage(false);
$preserved = xdebug_get_code_coverage();
var_dump(count_hits($preserved) > 0);      // preserved across stop(false)
?>
--CLEAN--
<?php
@unlink(__DIR__ . "/016_lib.php");
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
