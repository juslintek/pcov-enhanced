/*
  +----------------------------------------------------------------------+
  | pcov branch/path coverage producer                                   |
  +----------------------------------------------------------------------+
  | Route-independent producer: turns a zend_op_array into an            |
  | xdebug-compatible branch/path model and emits the array shape that   |
  | php-code-coverage's RawCodeCoverageData consumes.                    |
  |                                                                      |
  | The static analysis is a faithful port of Xdebug 3.6's              |
  | src/coverage/code_coverage.c (xdebug_find_jumps / analysis) and      |
  | src/coverage/branch_info.c (path enumeration). Branch ids are opcode |
  | indexes, exactly as Xdebug reports them, so numbers match.           |
  |                                                                      |
  | Runtime hit tracking is deferred: at runtime pcov only records which |
  | opcode indexes were reached (per op_array identity). Branch/out/path |
  | "hit" flags are computed at collect() time from that set. This keeps |
  | the hot path cheap, honoring pcov's low-overhead invariant.          |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_PCOV_BRANCH_H
#define PHP_PCOV_BRANCH_H

#include "zend.h"
#include "zend_compile.h"

/* Caps copied verbatim from xdebug src/coverage/branch_info.h so path
 * enumeration explodes and terminates identically. */
#define PCOV_MAX_PATHS        4096
#define PCOV_BRANCH_MAX_OUTS  64

/* Sentinels, mirroring XDEBUG_JMP_* */
#define PCOV_JMP_NOT_SET (INT_MAX - 1)
#define PCOV_JMP_EXIT    (INT_MAX - 2)

typedef struct _pcov_branch {
	uint32_t start_lineno;
	uint32_t end_lineno;
	uint32_t end_op;
	uint32_t outs_count;
	int      outs[PCOV_BRANCH_MAX_OUTS];
} pcov_branch;

typedef struct _pcov_path {
	uint32_t  elements_count;
	uint32_t  elements_size;
	uint32_t *elements;
} pcov_path;

/* One function's static branch/path analysis. Owns heap allocations. */
typedef struct _pcov_branch_info {
	uint32_t     size;          /* == op_array->last                       */
	pcov_branch *branches;      /* size entries; only "starts" are emitted */
	zend_bitset  starts;        /* opcode-index set: branch starts         */
	zend_bitset  ends;          /* opcode-index set: branch ends           */
	zend_bitset  entry_points;  /* opcode-index set: entry points          */
	size_t       highest_out;   /* max outs over all branches              */

	pcov_path  **paths;         /* enumerated paths                        */
	uint32_t     paths_count;
	uint32_t     paths_size;
} pcov_branch_info;

/* Build the static model for one op_array. Returns NULL for abstract funcs
 * or when analysis is not meaningful. Caller owns the result and frees it
 * with pcov_branch_info_free(). */
pcov_branch_info *pcov_branch_info_create_from_oparray(zend_op_array *op_array);

void pcov_branch_info_free(pcov_branch_info *info);

/*
 * Emit the xdebug-compatible per-function zval:
 *   [ "branches" => [...], "paths" => [...] ]
 * into z_function (must be an initialized array zval).
 *
 * reached is a zend_bitset over opcode indexes that were executed for this
 * op_array (or NULL, meaning "nothing hit" -> all hits 0). reached_len is the
 * number of bits (zend_bitset_len(op_array->last)).
 */
void pcov_branch_info_to_zval(
	zval *z_function,
	pcov_branch_info *info,
	const zend_bitset reached,
	uint32_t reached_bits);

/* Produce the canonical function key ("{main}", "Class->method", "func",
 * "{closure:file:line}") for an op_array, matching Xdebug's naming so
 * php-code-coverage keys line up. Writes into buf (NUL-terminated). */
void pcov_branch_function_key(char *buf, size_t buf_size, zend_op_array *op_array);

/* ---- runtime path recording support ------------------------------------- */

/* True if opcode index `idx` is the start of a branch in this analysis. */
static zend_always_inline int pcov_branch_is_start(pcov_branch_info *info, uint32_t idx) {
	return (idx < info->size) && zend_bitset_in(info->starts, idx);
}

/* Mark, in `hit_paths`, the enumerated path whose branch-id sequence equals
 * `seq` (length seq_len). Returns 1 if a matching path was found and marked.
 * hit_paths must be a zend_bitset of at least info->paths_count bits. This is
 * the pcov analogue of xdebug's mark_end_of_function_reached: only an exact
 * match of a statically enumerated path counts as a hit. */
int pcov_branch_mark_path_hit(pcov_branch_info *info, zend_bitset hit_paths,
                              const uint32_t *seq, uint32_t seq_len);

/* Edge index for a traversed-edge bitset: out-edge j of branch `br`. Mirrors
 * Xdebug's hit_branch encoding. Callers record edges at runtime with this and
 * pass the bitset to pcov_branch_info_to_zval_ex so out_hit is a real
 * observation, not an inference. */
static zend_always_inline uint32_t pcov_branch_edge_index(pcov_branch_info *info, uint32_t br, uint32_t j) {
	return br * (uint32_t)(1 + info->highest_out) + 1 + j;
}

/*
 * Emit branches/paths, taking explicit runtime hit sets:
 *   reached    : opcode-index bitset of reached branch starts (branch hit)
 *   path_hits  : path-index bitset of paths that were traversed as a unit
 *   edges      : traversed out-edge bitset (see pcov_branch_edge_index); an
 *                exit edge counts as taken when its branch was reached
 * Any may be NULL (meaning "nothing hit").
 */
void pcov_branch_info_to_zval_ex(
	zval *z_function,
	pcov_branch_info *info,
	const zend_bitset reached, uint32_t reached_bits,
	const zend_bitset path_hits, uint32_t path_hits_bits,
	const zend_bitset edges, uint32_t edges_bits);

#endif /* PHP_PCOV_BRANCH_H */
