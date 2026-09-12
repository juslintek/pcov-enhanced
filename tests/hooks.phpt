--TEST--
Property hooks
--SKIPIF--
<?php
if (!extension_loaded("pcov")) print "skip";
$__m = getenv("PCOV_MODE"); if ($__m === false || $__m === "") { $__m = (string) ini_get("pcov.mode"); } if (strcasecmp($__m, "branch") === 0 || strcasecmp($__m, "path") === 0) print "skip line-shape test not applicable in branch mode";
if (PHP_VERSION_ID < 80400) print "skip only for PHP >= 8.4";
?>
--INI--
pcov.enabled = 1
--FILE--
<?php
\pcov\start();
class MyClass {
  public int $myGetOnlyProp {
    get => 1;
  }

  public float $myGetSetProp {
    get {
      $tmp = $this->myGetSetProp;
      return $tmp * 2;
    }

    set(float $value) {
      $this->myGetSetProp = $value / 2;
    }
  }
}
$instance = new MyClass;
$instance->myGetOnlyProp;
$instance->myGetSetProp = 1;
$instance->myGetSetProp;
\pcov\stop();
var_dump(\pcov\collect());
?>
--EXPECTF--
array(1) {
  ["%s%ehooks.php"]=>
  array(13) {
    [2]=>
    int(-1)
    [19]=>
    int(1)
    [20]=>
    int(1)
    [21]=>
    int(1)
    [22]=>
    int(1)
    [23]=>
    int(1)
    [24]=>
    int(-1)
    [26]=>
    int(-1)
    [5]=>
    int(1)
    [10]=>
    int(1)
    [11]=>
    int(1)
    [15]=>
    int(1)
    [16]=>
    int(1)
  }
}
