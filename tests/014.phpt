--TEST--
branch mode: collect() returns branches and paths
--SKIPIF--
<?php
if (!extension_loaded("pcov")) print "skip pcov not loaded";
$__m = getenv("PCOV_MODE"); if ($__m === false || $__m === "") { $__m = (string) ini_get("pcov.mode"); }
if (strcasecmp($__m, "branch") !== 0 && strcasecmp($__m, "path") !== 0) print "skip requires branch mode";
?>
--INI--
pcov.enabled = 1
pcov.mode = branch
--FILE--
<?php
\pcov\start();
$c = static function (int $n): string {
    if ($n < 0) {
        return 'neg';
    } elseif ($n === 0) {
        return 'zero';
    }
    return 'pos';
};
$c(-1);
$c(0);
$c(5);
\pcov\stop();

$data = \pcov\collect();
$file = array_key_first($data);
$fileData = $data[$file];

// Branch mode returns the associative { lines, functions } shape.
var_dump(array_keys($fileData) === ['lines', 'functions']);

// The closure (the only code executed while coverage was active) must have
// branch and path coverage recorded, with every branch and at least one full
// path marked hit.
$closureKey = null;
foreach (array_keys($fileData['functions']) as $k) {
    if (strncmp($k, '{closure', 8) === 0) {
        $closureKey = $k;
        break;
    }
}

$fn = $fileData['functions'][$closureKey];

// Documented shape: each branch carries these keys.
$wellFormed = true;
$allBranchesHit = true;
foreach ($fn['branches'] as $b) {
    if (!isset($b['op_start'], $b['op_end'], $b['line_start'],
               $b['line_end'], $b['hit'], $b['out'], $b['out_hit'])) {
        $wellFormed = false;
    }
    if ($b['hit'] < 1) {
        $allBranchesHit = false;
    }
}

$anyPathHit = false;
foreach ($fn['paths'] as $p) {
    if (isset($p['path'], $p['hit']) && $p['hit'] >= 1) {
        $anyPathHit = true;
    }
}

var_dump(count($fn['branches']) > 1);
var_dump($wellFormed);
var_dump($allBranchesHit);
var_dump($anyPathHit);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
