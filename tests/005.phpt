--TEST--
anonymous class
--SKIPIF--
<?php
if (!extension_loaded("pcov")) print "skip";
$__m = getenv("PCOV_MODE"); if ($__m === false || $__m === "") { $__m = (string) ini_get("pcov.mode"); } if (strcasecmp($__m, "branch") === 0 || strcasecmp($__m, "path") === 0) print "skip line-shape test not applicable in branch mode";
?>
--INI--
pcov.enabled = 1
--FILE--
<?php
\pcov\start();
$a = new class() {};
\pcov\stop();
var_dump(\pcov\collect());
?>
--EXPECTF--
array(1) {
  ["%s%e005.php"]=>
  array(5) {
    [2]=>
    int(-1)
    [3]=>
    int(1)
    [4]=>
    int(1)
    [5]=>
    int(-1)
    [7]=>
    int(-1)
  }
}
