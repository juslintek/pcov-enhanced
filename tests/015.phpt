--TEST--
branch mode: xdebug_set_filter exclude honored via compat surface
--SKIPIF--
<?php
if (!extension_loaded("pcov")) print "skip pcov not loaded";
$__m = getenv("PCOV_MODE"); if ($__m === false || $__m === "") { $__m = (string) ini_get("pcov.mode"); }
if (strcasecmp($__m, "branch") !== 0 && strcasecmp($__m, "path") !== 0) print "skip requires branch mode";
if (!function_exists("xdebug_set_filter")) print "skip compat surface inactive (real xdebug present?)";
?>
--INI--
pcov.enabled = 1
pcov.mode = branch
pcov.directory = .
--FILE--
<?php
$dir = __DIR__;
file_put_contents($dir . "/015_keep.php", "<?php function k015(\$n){ if(\$n>0) return 1; return 0; }\n");
file_put_contents($dir . "/015_drop.php", "<?php function d015(\$n){ if(\$n>0) return 1; return 0; }\n");
require $dir . "/015_keep.php";
require $dir . "/015_drop.php";

xdebug_set_filter(XDEBUG_FILTER_CODE_COVERAGE, XDEBUG_PATH_EXCLUDE, [realpath($dir . "/015_drop.php")]);

xdebug_start_code_coverage(XDEBUG_CC_BRANCH_CHECK);
k015(1);
d015(1);
$data = xdebug_get_code_coverage();
xdebug_stop_code_coverage(false);

$hasKeep = false;
$hasDrop = false;
foreach ($data as $file => $_) {
    if (str_ends_with($file, "015_keep.php")) $hasKeep = true;
    if (str_ends_with($file, "015_drop.php")) $hasDrop = true;
}
var_dump($hasKeep);
var_dump($hasDrop);
?>
--CLEAN--
<?php
@unlink(__DIR__ . "/015_keep.php");
@unlink(__DIR__ . "/015_drop.php");
?>
--EXPECT--
bool(true)
bool(false)
