/*
  +----------------------------------------------------------------------+
  | pcov branch/path coverage producer  (see pcov_branch.h)              |
  +----------------------------------------------------------------------+
  | Static analysis ported from Xdebug 3.6 src/coverage/code_coverage.c  |
  | and src/coverage/branch_info.c. Branch ids are opcode indexes.       |
  +----------------------------------------------------------------------+
*/

#include "php.h"
#include "zend_compile.h"
#include "zend_bitset.h"
#include "pcov_branch.h"

#include <limits.h>

/* Mirror of XDEBUG_ZNODE_JMP_LINE: turn a jump znode into a target opcode
 * index. base_address only used in the absolute-jump build. */
#if ZEND_USE_ABS_JMP_ADDR
# define PCOV_ZNODE_JMP_LINE(node, opline, base) \
	(int32_t)(((long)((node).jmp_addr) - (long)(base)) / sizeof(zend_op))
#else
# define PCOV_ZNODE_JMP_LINE(node, opline, base) \
	(int32_t)(((int32_t)((node).jmp_offset) / (int32_t)sizeof(zend_op)) + (opline))
#endif

/* ---- path helpers (port of xdebug_path_*) -------------------------------- */

static pcov_path *pcov_path_new(pcov_path *old_path)
{
	pcov_path *tmp = ecalloc(1, sizeof(pcov_path));

	if (old_path) {
		uint32_t i;
		for (i = 0; i < old_path->elements_count; i++) {
			if (tmp->elements_count == tmp->elements_size) {
				tmp->elements_size += 32;
				tmp->elements = erealloc(tmp->elements, sizeof(uint32_t) * tmp->elements_size);
			}
			tmp->elements[tmp->elements_count++] = old_path->elements[i];
		}
	}
	return tmp;
}

static void pcov_path_add(pcov_path *path, uint32_t nr)
{
	if (!path) {
		return;
	}
	if (path->elements_count == path->elements_size) {
		path->elements_size += 32;
		path->elements = erealloc(path->elements, sizeof(uint32_t) * path->elements_size);
	}
	path->elements[path->elements_count++] = nr;
}

static void pcov_path_free(pcov_path *path)
{
	if (path->elements) {
		efree(path->elements);
	}
	efree(path);
}

static void pcov_path_info_add_path(pcov_branch_info *info, pcov_path *path)
{
	if (info->paths_count == info->paths_size) {
		info->paths_size += 32;
		info->paths = erealloc(info->paths, sizeof(pcov_path *) * info->paths_size);
	}
	info->paths[info->paths_count++] = path;
}

static uint32_t pcov_path_last(pcov_path *path)
{
	return path->elements[path->elements_count - 1];
}

/* Has edge (elem1 -> elem2) already been walked in this path? */
static int pcov_path_edge_exists(pcov_path *path, uint32_t elem1, uint32_t elem2)
{
	uint32_t i;
	if (path->elements_count < 2) {
		return 0;
	}
	for (i = 0; i < path->elements_count - 1; i++) {
		if (path->elements[i] == elem1 && path->elements[i + 1] == elem2) {
			return 1;
		}
	}
	return 0;
}

/* DFS enumeration, self-avoiding per edge (port of xdebug_branch_find_path). */
static void pcov_branch_find_path(uint32_t nr, pcov_branch_info *info, pcov_path *prev_path)
{
	uint32_t last;
	pcov_path *new_path;
	int found = 0;
	size_t i;

	/* nr indexes info->branches, which holds exactly info->size entries. */
	if (nr >= info->size) {
		return;
	}

	if (info->paths_count >= PCOV_MAX_PATHS) {
		return;
	}

	new_path = pcov_path_new(prev_path);
	pcov_path_add(new_path, nr);

	last = pcov_path_last(new_path);

	for (i = 0; i < info->branches[nr].outs_count; i++) {
		int out = info->branches[nr].outs[i];
		/* Only follow real, in-range targets. A jump-derived target can exceed
		 * info->size on a malformed/edge-case CFG; the recursive call also
		 * re-checks, but guard here so the enumeration never recurses on an
		 * out-of-range index. */
		if (out > 0 && (uint32_t) out < info->size && out != PCOV_JMP_EXIT &&
		    !pcov_path_edge_exists(new_path, last, (uint32_t) out)) {
			pcov_branch_find_path((uint32_t) out, info, new_path);
			found = 1;
		}
	}

	if (found) {
		pcov_path_free(new_path);
		return;
	}

	pcov_path_info_add_path(info, new_path);
}

static void pcov_branch_find_paths(pcov_branch_info *info)
{
	uint32_t i;
	for (i = 0; i < info->size; i++) {
		if (zend_bitset_in(info->entry_points, i)) {
			pcov_branch_find_path(i, info, NULL);
		}
	}
}

/* ---- static branch analysis (port of xdebug_find_jumps/analysis) --------- */

static void pcov_branch_info_update(pcov_branch_info *info, uint32_t pos,
                                    uint32_t lineno, uint32_t outidx, int jump_pos)
{
	zend_bitset_incl(info->ends, pos);
	if (outidx < PCOV_BRANCH_MAX_OUTS) {
		info->branches[pos].outs[outidx] = jump_pos;
		if (outidx + 1 > info->branches[pos].outs_count) {
			info->branches[pos].outs_count = outidx + 1;
		}
	}
	info->branches[pos].start_lineno = lineno;
}

static int pcov_find_jumps(zend_op_array *opa, uint32_t position, size_t *jump_count, int *jumps)
{
#if ZEND_USE_ABS_JMP_ADDR
	zend_op *base_address = &(opa->opcodes[0]);
#else
	zend_op *base_address = NULL;
#endif
	zend_op opcode = opa->opcodes[position];

	if (opcode.opcode == ZEND_JMP) {
		jumps[0] = PCOV_ZNODE_JMP_LINE(opcode.op1, position, base_address);
		*jump_count = 1;
		return 1;
	} else if (
		opcode.opcode == ZEND_JMPZ ||
		opcode.opcode == ZEND_JMPNZ ||
		opcode.opcode == ZEND_JMPZ_EX ||
		opcode.opcode == ZEND_JMPNZ_EX
	) {
		jumps[0] = position + 1;
		jumps[1] = PCOV_ZNODE_JMP_LINE(opcode.op2, position, base_address);
		*jump_count = 2;
		return 1;
	} else if (opcode.opcode == ZEND_FE_FETCH_R || opcode.opcode == ZEND_FE_FETCH_RW) {
		jumps[0] = position + 1;
		jumps[1] = position + (opcode.extended_value / sizeof(zend_op));
		*jump_count = 2;
		return 1;
	} else if (opcode.opcode == ZEND_FE_RESET_R || opcode.opcode == ZEND_FE_RESET_RW) {
		jumps[0] = position + 1;
		jumps[1] = PCOV_ZNODE_JMP_LINE(opcode.op2, position, base_address);
		*jump_count = 2;
		return 1;
	} else if (opcode.opcode == ZEND_CATCH) {
		*jump_count = 2;
		jumps[0] = position + 1;
		if (!(opcode.extended_value & ZEND_LAST_CATCH)) {
			jumps[1] = PCOV_ZNODE_JMP_LINE(opcode.op2, position, base_address);
			if (jumps[1] == jumps[0]) {
				jumps[1] = PCOV_JMP_NOT_SET;
				*jump_count = 1;
			}
		} else {
			jumps[1] = PCOV_JMP_EXIT;
		}
		return 1;
	} else if (opcode.opcode == ZEND_GOTO) {
		jumps[0] = PCOV_ZNODE_JMP_LINE(opcode.op1, position, base_address);
		*jump_count = 1;
		return 1;
	} else if (opcode.opcode == ZEND_FAST_CALL) {
		jumps[0] = PCOV_ZNODE_JMP_LINE(opcode.op1, position, base_address);
		jumps[1] = position + 1;
		*jump_count = 2;
		return 1;
	} else if (opcode.opcode == ZEND_FAST_RET) {
		jumps[0] = PCOV_JMP_EXIT;
		*jump_count = 1;
		return 1;
#if PHP_VERSION_ID >= 80400
	} else if (opcode.opcode == ZEND_JMP_FRAMELESS) {
		jumps[0] = position + 1;
		jumps[1] = PCOV_ZNODE_JMP_LINE(opcode.op2, position, base_address);
		*jump_count = 2;
		return 1;
#endif
	} else if (
		opcode.opcode == ZEND_GENERATOR_RETURN ||
#if PHP_VERSION_ID < 80400
		opcode.opcode == ZEND_EXIT ||
#endif
		opcode.opcode == ZEND_THROW ||
		opcode.opcode == ZEND_MATCH_ERROR ||
		opcode.opcode == ZEND_RETURN
	) {
		jumps[0] = PCOV_JMP_EXIT;
		*jump_count = 1;
		return 1;
	} else if (opcode.opcode == ZEND_INIT_FCALL) {
		zval *func_name = RT_CONSTANT(&opa->opcodes[position], opcode.op2);
		if (func_name && Z_TYPE_P(func_name) == IS_STRING &&
		    zend_string_equals_literal(Z_STR_P(func_name), "exit")) {
			int level = 0;
			uint32_t start = position + 1;
			for (;;) {
				if (start >= opa->last) {
					break;
				}
				switch (opa->opcodes[start].opcode) {
					case ZEND_INIT_FCALL:
					case ZEND_INIT_FCALL_BY_NAME:
					case ZEND_INIT_NS_FCALL_BY_NAME:
					case ZEND_INIT_DYNAMIC_CALL:
					case ZEND_INIT_USER_CALL:
					case ZEND_INIT_METHOD_CALL:
					case ZEND_INIT_STATIC_METHOD_CALL:
#if PHP_VERSION_ID >= 80400
					case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
#endif
					case ZEND_NEW:
						level++;
						break;
					case ZEND_DO_FCALL:
					case ZEND_DO_FCALL_BY_NAME:
					case ZEND_DO_ICALL:
					case ZEND_DO_UCALL:
						if (level == 0) {
							goto done;
						}
						level--;
						break;
				}
				start++;
			}
 done:
			jumps[0] = PCOV_JMP_EXIT;
			*jump_count = 1;
			return 1;
		}
	} else if (
		opcode.opcode == ZEND_MATCH ||
		opcode.opcode == ZEND_SWITCH_LONG ||
		opcode.opcode == ZEND_SWITCH_STRING
	) {
		zval *array_value;
		HashTable *myht;
		zval *val;

		array_value = RT_CONSTANT(&opa->opcodes[position], opcode.op2);
		myht = Z_ARRVAL_P(array_value);

		ZEND_HASH_FOREACH_VAL_IND(myht, val) {
			if (*jump_count < PCOV_BRANCH_MAX_OUTS - 2) {
				jumps[*jump_count] = position + (val->value.lval / sizeof(zend_op));
				(*jump_count)++;
			}
		} ZEND_HASH_FOREACH_END();

		jumps[*jump_count] = position + (opcode.extended_value / sizeof(zend_op));
		(*jump_count)++;

		if (opcode.opcode != ZEND_MATCH) {
			jumps[*jump_count] = position + 1;
			(*jump_count)++;
		}
		return 1;
	}

	return 0;
}

static void pcov_analysis_branch(zend_op_array *opa, uint32_t position, zend_bitset set, pcov_branch_info *info)
{
	/* position may be a jump-target index derived from an opcode operand;
	 * refuse anything outside the analysed op_array so the writes below and the
	 * opcode reads in the loop cannot exceed the (op_array->last) allocations. */
	if (position >= info->size || position >= opa->last) {
		return;
	}

	zend_bitset_incl(info->starts, position);
	info->branches[position].start_lineno = opa->opcodes[position].lineno;

	if (zend_bitset_in(set, position)) {
		return;
	}

	zend_bitset_incl(set, position);
	while (position < opa->last) {
		size_t jump_count = 0;
		int    jumps[PCOV_BRANCH_MAX_OUTS];
		size_t i;

		if (pcov_find_jumps(opa, position, &jump_count, jumps)) {
			if (jump_count > info->highest_out) {
				info->highest_out = jump_count;
			}
			for (i = 0; i < jump_count; i++) {
				if (jumps[i] == PCOV_JMP_EXIT || jumps[i] != PCOV_JMP_NOT_SET) {
					pcov_branch_info_update(info, position, opa->opcodes[position].lineno, i, jumps[i]);
					if (jumps[i] != PCOV_JMP_EXIT) {
						pcov_analysis_branch(opa, (uint32_t) jumps[i], set, info);
					}
				}
			}
			break;
		}

		if (opa->opcodes[position].opcode == ZEND_THROW) {
			zend_bitset_incl(info->ends, position);
			info->branches[position].start_lineno = opa->opcodes[position].lineno;
			break;
		}
#if PHP_VERSION_ID < 80400
		if (opa->opcodes[position].opcode == ZEND_EXIT) {
			zend_bitset_incl(info->ends, position);
			info->branches[position].start_lineno = opa->opcodes[position].lineno;
			break;
		}
#endif
		if (opa->opcodes[position].opcode == ZEND_RETURN ||
		    opa->opcodes[position].opcode == ZEND_RETURN_BY_REF) {
			zend_bitset_incl(info->ends, position);
			info->branches[position].start_lineno = opa->opcodes[position].lineno;
			break;
		}

		position++;
		zend_bitset_incl(set, position);
	}
}

static void pcov_analysis_oparray(zend_op_array *opa, zend_bitset set, pcov_branch_info *info)
{
	uint32_t position = 0;

	while (position < opa->last) {
		if (position == 0) {
			pcov_analysis_branch(opa, position, set, info);
			zend_bitset_incl(info->entry_points, position);
		} else if (opa->opcodes[position].opcode == ZEND_CATCH) {
			pcov_analysis_branch(opa, position, set, info);
			zend_bitset_incl(info->entry_points, position);
		}
		position++;
	}
	zend_bitset_incl(info->ends, opa->last - 1);
	info->branches[opa->last - 1].start_lineno = opa->opcodes[opa->last - 1].lineno;
}

/* ---- post-process (port of xdebug_branch_post_process) ------------------- */

static void pcov_only_leave_first_catch(zend_op_array *opa, pcov_branch_info *info, int position)
{
	uint32_t exit_jmp;
#if ZEND_USE_ABS_JMP_ADDR
	zend_op *base_address = &(opa->opcodes[0]);
#else
	zend_op *base_address = NULL;
#endif

	/* position and every index derived from a jump operand below are validated
	 * against opa->last before dereferencing opa->opcodes, including after the
	 * position++ / exit_jmp++ increments. */
	if (position < 0 || (uint32_t) position >= opa->last) {
		return;
	}
	if (opa->opcodes[position].opcode == ZEND_FETCH_CLASS) {
		position++;
		if ((uint32_t) position >= opa->last) {
			return;
		}
	}
	if (opa->opcodes[position].opcode != ZEND_CATCH) {
		return;
	}
	zend_bitset_excl(info->entry_points, position);

	if (opa->opcodes[position].extended_value & ZEND_LAST_CATCH) {
		return;
	}
	exit_jmp = PCOV_ZNODE_JMP_LINE(opa->opcodes[position].op2, position, base_address);
	if (exit_jmp >= opa->last) {
		return;
	}
	if (opa->opcodes[exit_jmp].opcode == ZEND_FETCH_CLASS) {
		exit_jmp++;
		if (exit_jmp >= opa->last) {
			return;
		}
	}
	if (opa->opcodes[exit_jmp].opcode == ZEND_CATCH) {
		pcov_only_leave_first_catch(opa, info, exit_jmp);
	}
}

static void pcov_branch_post_process(zend_op_array *opa, pcov_branch_info *info)
{
	uint32_t i;
	int in_branch = 0, last_start = -1;
#if ZEND_USE_ABS_JMP_ADDR
	zend_op *base_address = &(opa->opcodes[0]);
#else
	zend_op *base_address = NULL;
#endif

	for (i = 0; i < info->size; i++) {
		if (zend_bitset_in(info->entry_points, i) && opa->opcodes[i].opcode == ZEND_CATCH) {
			/* A LAST_CATCH terminates the chain and is always a real entry
			 * point; do not follow op2 to prune it. (Xdebug relies on the
			 * last catch's op2 being unset here; pcov compiles op_arrays
			 * without jumptables, which can leave a non-zero op2 offset on the
			 * last catch, so we gate on the flag instead.) */
			if (opa->opcodes[i].extended_value & ZEND_LAST_CATCH) {
				continue;
			}
#if ZEND_USE_ABS_JMP_ADDR
			if (opa->opcodes[i].op2.jmp_addr != (void *) -1) {
#else
			if (opa->opcodes[i].op2.jmp_offset != 0) {
#endif
				pcov_only_leave_first_catch(opa, info, PCOV_ZNODE_JMP_LINE(opa->opcodes[i].op2, i, base_address));
			}
		}
	}

	for (i = 0; i < info->size; i++) {
		if (zend_bitset_in(info->starts, i)) {
			if (in_branch) {
				info->branches[last_start].outs_count = 1;
				info->branches[last_start].outs[0] = i;
				info->branches[last_start].end_op = i - 1;
				info->branches[last_start].end_lineno = info->branches[i].start_lineno;
			}
			last_start = i;
			in_branch = 1;
		}
		if (zend_bitset_in(info->ends, i)) {
			size_t j;
			for (j = 0; j < info->branches[i].outs_count; j++) {
				info->branches[last_start].outs[j] = info->branches[i].outs[j];
			}
			info->branches[last_start].outs_count = info->branches[i].outs_count;
			info->branches[last_start].end_op = i;
			info->branches[last_start].end_lineno = info->branches[i].start_lineno;
			in_branch = 0;
		}
	}
}

/* ---- lifecycle ----------------------------------------------------------- */

pcov_branch_info *pcov_branch_info_create_from_oparray(zend_op_array *op_array)
{
	pcov_branch_info *info;
	zend_bitset set;
	uint32_t blen;

	if (op_array->fn_flags & ZEND_ACC_ABSTRACT) {
		return NULL;
	}
	if (op_array->last == 0 || op_array->opcodes == NULL) {
		return NULL;
	}

	info = ecalloc(1, sizeof(pcov_branch_info));
	info->size = op_array->last;
	info->branches = ecalloc(info->size, sizeof(pcov_branch));

	blen = zend_bitset_len(info->size);
	info->starts       = ecalloc(blen, sizeof(zend_ulong));
	info->ends         = ecalloc(blen, sizeof(zend_ulong));
	info->entry_points = ecalloc(blen, sizeof(zend_ulong));
	info->highest_out  = 0;

	set = ecalloc(blen, sizeof(zend_ulong));
	pcov_analysis_oparray(op_array, set, info);
	efree(set);

	pcov_branch_post_process(op_array, info);
	pcov_branch_find_paths(info);

	return info;
}

void pcov_branch_info_free(pcov_branch_info *info)
{
	uint32_t i;
	if (!info) {
		return;
	}
	for (i = 0; i < info->paths_count; i++) {
		pcov_path_free(info->paths[i]);
	}
	if (info->paths) {
		efree(info->paths);
	}
	efree(info->branches);
	efree(info->starts);
	efree(info->ends);
	efree(info->entry_points);
	efree(info);
}

/* ---- output (port of add_branches / add_paths) --------------------------- */

/* A branch id is "hit" iff its start opcode index was reached. An out-edge
 * (i -> outs[j]) is "hit" iff both endpoints were reached; this matches how
 * xdebug's runtime records an out only when the successor branch is entered
 * from this branch. Since a branch chain is contiguous, reaching the start op
 * of both branches is the observable equivalent. */
static int pcov_reached(const zend_bitset reached, uint32_t reached_bits, uint32_t idx)
{
	if (!reached) {
		return 0;
	}
	if (idx >= reached_bits) {
		return 0;
	}
	return zend_bitset_in((zend_bitset) reached, idx) ? 1 : 0;
}

static void pcov_add_branches(zval *z_function, pcov_branch_info *info,
                              const zend_bitset reached, uint32_t reached_bits,
                              const zend_bitset edges, uint32_t edges_bits)
{
	zval branches, branch, out, out_hit;
	uint32_t i;

	array_init(&branches);

	for (i = 0; i < info->size; i++) {
		if (zend_bitset_in(info->starts, i)) {
			size_t j;
			int branch_hit = pcov_reached(reached, reached_bits, i);

			array_init(&branch);
			add_assoc_long(&branch, "op_start", i);
			add_assoc_long(&branch, "op_end", info->branches[i].end_op);
			add_assoc_long(&branch, "line_start", info->branches[i].start_lineno);
			add_assoc_long(&branch, "line_end", info->branches[i].end_lineno);
			add_assoc_long(&branch, "hit", branch_hit);

			array_init(&out);
			for (j = 0; j < info->branches[i].outs_count; j++) {
				if (info->branches[i].outs[j]) {
					add_index_long(&out, j, info->branches[i].outs[j]);
				}
			}
			add_assoc_zval(&branch, "out", &out);

			array_init(&out_hit);
			for (j = 0; j < info->branches[i].outs_count; j++) {
				if (info->branches[i].outs[j]) {
					int oh = 0;
					/* Both real and EXIT edges are reported from the recorded
					 * runtime edge set. EXIT edges are recorded at frame
					 * finalize only for the branch execution actually left
					 * through, so a ZEND_LAST_CATCH whose catch matched (and
					 * continued at the fall-through) does not falsely report its
					 * exit edge as taken. Fall back to branch_hit only when no
					 * edge set is available (e.g. static/uncovered emission). */
					if (edges) {
						uint32_t ei = pcov_branch_edge_index(info, i, (uint32_t) j);
						oh = (ei < edges_bits && zend_bitset_in((zend_bitset) edges, ei)) ? 1 : 0;
					} else {
						oh = 0;
					}
					add_index_long(&out_hit, j, oh);
				}
			}
			add_assoc_zval(&branch, "out_hit", &out_hit);

			add_index_zval(&branches, i, &branch);
		}
	}

	add_assoc_zval_ex(z_function, "branches", sizeof("branches") - 1, &branches);
}

static void pcov_add_paths(zval *z_function, pcov_branch_info *info,
                           const zend_bitset path_hits, uint32_t path_hits_bits)
{
	zval paths, path, path_container;
	uint32_t i, j;

	array_init(&paths);

	for (i = 0; i < info->paths_count; i++) {
		int path_hit = 0;
		pcov_path *p = info->paths[i];

		if (path_hits && i < path_hits_bits && zend_bitset_in((zend_bitset) path_hits, i)) {
			path_hit = 1;
		}

		array_init(&path);
		array_init(&path_container);

		for (j = 0; j < p->elements_count; j++) {
			add_next_index_long(&path, p->elements[j]);
		}

		add_assoc_zval(&path_container, "path", &path);
		add_assoc_long(&path_container, "hit", path_hit);

		add_next_index_zval(&paths, &path_container);
	}

	add_assoc_zval_ex(z_function, "paths", sizeof("paths") - 1, &paths);
}

/* Compare two branch-id sequences for equality. */
static int pcov_seq_equals(pcov_path *p, const uint32_t *seq, uint32_t seq_len)
{
	uint32_t j;
	if (p->elements_count != seq_len) {
		return 0;
	}
	for (j = 0; j < seq_len; j++) {
		if (p->elements[j] != seq[j]) {
			return 0;
		}
	}
	return 1;
}

int pcov_branch_mark_path_hit(pcov_branch_info *info, zend_bitset hit_paths,
                              const uint32_t *seq, uint32_t seq_len)
{
	uint32_t i;
	if (seq_len == 0) {
		return 0;
	}
	for (i = 0; i < info->paths_count; i++) {
		if (pcov_seq_equals(info->paths[i], seq, seq_len)) {
			zend_bitset_incl(hit_paths, i);
			return 1;
		}
	}
	return 0;
}

void pcov_branch_info_to_zval_ex(zval *z_function, pcov_branch_info *info,
                                 const zend_bitset reached, uint32_t reached_bits,
                                 const zend_bitset path_hits, uint32_t path_hits_bits,
                                 const zend_bitset edges, uint32_t edges_bits)
{
	pcov_add_branches(z_function, info, reached, reached_bits, edges, edges_bits);
	pcov_add_paths(z_function, info, path_hits, path_hits_bits);
}

void pcov_branch_info_to_zval(zval *z_function, pcov_branch_info *info,
                              const zend_bitset reached, uint32_t reached_bits)
{
	pcov_branch_info_to_zval_ex(z_function, info, reached, reached_bits, NULL, 0, NULL, 0);
}

/* ---- function key naming (port of xdebug_build_fname_from_oparray) ------- */

void pcov_branch_function_key(char *buf, size_t buf_size, zend_op_array *op_array)
{
	if (!op_array->function_name) {
		snprintf(buf, buf_size, "{main}");
		return;
	}

	if (op_array->fn_flags & ZEND_ACC_CLOSURE) {
		snprintf(buf, buf_size, "{closure:%s:%d}",
			op_array->filename ? ZSTR_VAL(op_array->filename) : "?",
			(int) op_array->line_start);
		return;
	}

	if (op_array->scope) {
		snprintf(buf, buf_size, "%s->%s",
			ZSTR_VAL(op_array->scope->name), ZSTR_VAL(op_array->function_name));
		return;
	}

	snprintf(buf, buf_size, "%s", ZSTR_VAL(op_array->function_name));
}
