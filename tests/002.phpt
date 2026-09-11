--TEST--
clear
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
$d = [];
for ($i = 0; $i < 10; $i++) {
	$d[] = $i * 42;
}
\pcov\stop();

var_dump(\pcov\collect());

\pcov\clear();

var_dump(\pcov\collect());
?>
--EXPECTF--
array(1) {
  ["%s%e002.php"]=>
  array(9) {
    [2]=>
    int(-1)
    [3]=>
    int(1)
    [4]=>
    int(1)
    [5]=>
    int(1)
    [7]=>
    int(1)
    [9]=>
    int(-1)
    [11]=>
    int(-1)
    [13]=>
    int(-1)
    [15]=>
    int(-1)
  }
}
array(0) {
}


