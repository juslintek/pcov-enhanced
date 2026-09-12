/*
  +----------------------------------------------------------------------+
  | Copyright (c) The PHP Group                                          |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: krakjoe                                                      |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/pcre/php_pcre.h"

#include "zend_arena.h"
#if PHP_VERSION_ID < 80100
# include "zend_cfg.h"
# define PHP_PCOV_CFG ZEND_RT_CONSTANTS
#else
# include "Zend/Optimizer/zend_cfg.h"
# define PHP_PCOV_CFG 0
#endif
#include "zend_bitset.h"
#include "zend_exceptions.h"
#include "zend_vm.h"
#include "zend_vm_opcodes.h"

#include "php_pcov.h"
#include "pcov_branch.h"
#include "pcov_xdebug_compat.h"

#define PCOV_FILTER_ALL     0
#define PCOV_FILTER_INCLUDE 1
#define PCOV_FILTER_EXCLUDE 2

#define PHP_PCOV_UNCOVERED   -1
#define PHP_PCOV_COVERED      1

#ifndef GC_ADDREF
#	define GC_ADDREF(g) ++GC_REFCOUNT(g)
#endif

#if PHP_VERSION_ID < 70300
#define php_pcre_pce_incref(c) (c)->refcount++
#define php_pcre_pce_decref(c) (c)->refcount--
#define GC_SET_REFCOUNT(ref, rc) (GC_REFCOUNT(ref) = (rc))
#endif

static zend_always_inline bool php_pcov_api_enabled(void) {
	const char* env = getenv("PCOV_ENABLED");

	if (env) {
		size_t length = strlen(env);

		if (length == 1) {
			switch (*env) {
				case '1':
				case 'y': /* short yes */
					return true;
				case '0':
				case 'n': /* short no */
					return false;
			}
			/* all other characters are assumed false */
			return false;
		}

		if (length == 2) {
			if (strncasecmp(env, ZEND_STRL("on")) == SUCCESS) {
				return true;
			} else if (strncasecmp(env, ZEND_STRL("no")) == SUCCESS) {
				return false;
			}

			/* all other strings are assumed false */
			return false;
		}

		if (length == 3) {
			if (strncasecmp(env, ZEND_STRL("off")) == SUCCESS) {
				return false;
			} else if (strncasecmp(env, ZEND_STRL("yes")) == SUCCESS) {
				return true;
			}

			/* all other strings are assumed false */
			return false;
		}

		/* all other lengths are assumed false */
		return false;
	}

	return INI_BOOL("pcov.enabled");
}

#define PHP_PCOV_API_ENABLED_GUARD() do { \
	if (!php_pcov_api_enabled()) { \
		return; \
	} \
} while (0);

static zval php_pcov_uncovered;
static zval php_pcov_covered;
static zend_ulong php_pcov_opcode_lut[256 / ZEND_BITSET_ELM_SIZE];

void (*zend_execute_ex_function)(zend_execute_data *execute_data);
zend_op_array* (*zend_compile_file_function)(zend_file_handle *fh, int type) = NULL;

ZEND_DECLARE_MODULE_GLOBALS(pcov)

PHP_INI_BEGIN()
	STD_PHP_INI_BOOLEAN(
		"pcov.enabled", "1",
		PHP_INI_SYSTEM, OnUpdateBool,
		ini.enabled, zend_pcov_globals, pcov_globals)
	STD_PHP_INI_ENTRY  (
		"pcov.directory", "",
		PHP_INI_SYSTEM | PHP_INI_PERDIR, OnUpdateString,
		ini.directory, zend_pcov_globals, pcov_globals)
	STD_PHP_INI_ENTRY  (
		"pcov.exclude", "",
		PHP_INI_SYSTEM | PHP_INI_PERDIR, OnUpdateString,
		ini.exclude, zend_pcov_globals, pcov_globals)
	STD_PHP_INI_ENTRY(
		"pcov.initial.memory", "65336",
		PHP_INI_SYSTEM | PHP_INI_PERDIR, OnUpdateLong,
		ini.memory, zend_pcov_globals, pcov_globals)
	STD_PHP_INI_ENTRY(
		"pcov.initial.files", "64",
		PHP_INI_SYSTEM | PHP_INI_PERDIR, OnUpdateLong,
		ini.files, zend_pcov_globals, pcov_globals)
	STD_PHP_INI_ENTRY  (
		/* SYSTEM-only: the xdebug-compat surface is decided at MINIT, before
		 * CGI/FastCGI applies per-directory (.user.ini) values. Allowing PERDIR
		 * here would let .user.ini request branch mode at RINIT while the fake
		 * xdebug module / xdebug_* functions were never registered, so PHPUnit
		 * could not select its path-coverage driver. Keep it a startup setting. */
		"pcov.mode", "line",
		PHP_INI_SYSTEM, OnUpdateString,
		ini.mode, zend_pcov_globals, pcov_globals)
PHP_INI_END()

/* Resolve pcov.mode / PCOV_MODE env into PCOV_MODE_*. Branch mode is strictly
 * opt-in; anything unrecognized is line mode, so default behavior never
 * changes. */
static zend_always_inline int php_pcov_resolve_mode(void) {
	const char *env = getenv("PCOV_MODE");
	/* An exported-but-empty PCOV_MODE is treated as unset, falling back to
	 * pcov.mode (the .phpt guards use the same rule). */
	const char *mode = (env && *env) ? env : INI_STR("pcov.mode");

	if (mode && (strcasecmp(mode, "branch") == 0 || strcasecmp(mode, "path") == 0)) {
		/* Guardrail: if the real Xdebug extension is present, it owns coverage
		 * instrumentation. Running pcov's branch tracking alongside it doubles
		 * the opcode instrumentation and can corrupt state, so we downgrade to
		 * line mode and let Xdebug provide branch/path coverage. (The
		 * xdebug-compat shim also declines to register in this case.) */
		if (zend_hash_str_exists(&module_registry, "xdebug", sizeof("xdebug") - 1) &&
		    !php_pcov_xdebug_compat_active()) {
			return PCOV_MODE_LINE;
		}
		return PCOV_MODE_BRANCH;
	}
	return PCOV_MODE_LINE;
}

/*
 * Per-op_array runtime cache (branch mode). Keyed in PCG(reached) by the
 * op_array's opcodes pointer (stable across the request while the op_array
 * lives, which pcov guarantees by refcounting them in PCG(files)).
 *
 * Holds:
 *   info      : the static branch/path analysis, built once per op_array.
 *   reached   : opcode-index bitset of branch starts entered (branch hit).
 *   path_hits : path-index bitset of enumerated paths traversed as a unit.
 */
typedef struct _php_pcov_reached_t {
	pcov_branch_info *info;
	zend_bitset       reached;      /* size zend_bitset_len(nops) */
	uint32_t          reached_bits; /* == op_array->last */
	zend_bitset       path_hits;    /* size zend_bitset_len(paths_count) */
	uint32_t          path_hits_bits;
	zend_bitset       edges;        /* traversed out-edges (see edge index)   */
	uint32_t          edges_bits;   /* size*(1+highest_out)                    */
	/* Identity of the op_array this entry was built for. The cache is keyed by
	 * the (opcodes) address, which Zend can reuse after a transient op_array is
	 * freed; these let us detect a stale hit and rebuild instead of returning
	 * a mismatched CFG/hit-set. */
	zend_string      *id_filename;  /* borrowed ref to op_array->filename     */
	uint32_t          id_line_start;
	uint32_t          id_last;
} php_pcov_reached_t;

static void php_pcov_reached_dtor(zval *zv) {
	php_pcov_reached_t *r = (php_pcov_reached_t *) Z_PTR_P(zv);
	if (r->info) {
		pcov_branch_info_free(r->info);
	}
	if (r->reached) {
		efree(r->reached);
	}
	if (r->path_hits) {
		efree(r->path_hits);
	}
	if (r->edges) {
		efree(r->edges);
	}
	if (r->id_filename) {
		zend_string_release(r->id_filename);
	}
	efree(r);
}

/* Finalize and detach any live frame whose cached analysis is `r`, so the
 * entry can be freed without leaving a dangling pcov_frame_t.cache. Defined
 * after the frame stack; forward-declared here. */
static void php_pcov_frames_detach_cache(php_pcov_reached_t *r);

/* Get-or-build the cache entry for an op_array. NULL if not analyzable. */
static php_pcov_reached_t *php_pcov_reached_get(zend_op_array *op_array) {
	php_pcov_reached_t *r;
	zend_ulong key = (zend_ulong) (uintptr_t) op_array->opcodes;

	/* Do not cache transient op_arrays. Eval code (and other non-user code) is
	 * destroyed after execution, after which its `opcodes` address may be
	 * reused by a different op_array; a cache keyed on that address would then
	 * return a stale CFG/hit-set. pcov only retains ZEND_USER_FUNCTION
	 * op_arrays (refcounted in PCG(files) / the function tables) for the whole
	 * request, so restrict caching to those. The runtime trace hook already
	 * gates on the same type. */
	if (op_array->type != ZEND_USER_FUNCTION) {
		return NULL;
	}

	r = zend_hash_index_find_ptr(&PCG(reached), key);
	if (EXPECTED(r)) {
		/* Validate that this cache entry still belongs to the same op_array:
		 * the keyed `opcodes` address can be reused after a transient op_array
		 * (e.g. a function defined inside an include/eval) is freed. If the
		 * identity differs, the entry is stale — evict and rebuild. */
		if (r->id_last == op_array->last &&
		    r->id_line_start == (uint32_t) op_array->line_start &&
		    ((r->id_filename == NULL && op_array->filename == NULL) ||
		     (r->id_filename != NULL && op_array->filename != NULL &&
		      zend_string_equals(r->id_filename, op_array->filename)))) {
			return r;
		}
		/* A returned-but-not-yet-finalized frame in PCG(frames) may still hold
		 * this entry as a raw `cache` pointer. Finalize+detach those frames
		 * BEFORE the dtor frees the entry, so php_pcov_frame_finalize() can
		 * never dereference freed memory through cache->info / cache->edges. */
		php_pcov_frames_detach_cache(r);
		zend_hash_index_del(&PCG(reached), key); /* dtor frees the stale entry */
	}

	if (op_array->last == 0 || (op_array->fn_flags & ZEND_ACC_ABSTRACT)) {
		return NULL;
	}

	r = ecalloc(1, sizeof(php_pcov_reached_t));
	r->info = pcov_branch_info_create_from_oparray(op_array);
	if (!r->info) {
		efree(r);
		return NULL;
	}
	r->reached_bits   = op_array->last;
	r->reached        = ecalloc(zend_bitset_len(r->reached_bits), sizeof(zend_ulong));
	r->path_hits_bits = r->info->paths_count;
	r->path_hits      = r->path_hits_bits
		? ecalloc(zend_bitset_len(r->path_hits_bits), sizeof(zend_ulong))
		: NULL;
	r->edges_bits     = r->info->size * (uint32_t)(1 + r->info->highest_out) + 1;
	r->edges          = ecalloc(zend_bitset_len(r->edges_bits), sizeof(zend_ulong));
	r->id_filename    = op_array->filename ? zend_string_copy(op_array->filename) : NULL;
	r->id_line_start  = (uint32_t) op_array->line_start;
	r->id_last        = op_array->last;

	zend_hash_index_add_ptr(&PCG(reached), key, r);
	return r;
}

static zend_always_inline php_pcov_reached_t *php_pcov_reached_find(zend_op_array *op_array) {
	zend_ulong key = (zend_ulong) (uintptr_t) op_array->opcodes;
	return zend_hash_index_find_ptr(&PCG(reached), key);
}

/*
 * Runtime frame stack for per-invocation path recording. Each live PHP frame
 * that we are tracking has an entry recording the ordered sequence of branch
 * starts entered (deduplicated against the immediately previous branch, as
 * Xdebug does). On frame exit the sequence is matched against the function's
 * enumerated paths and, on exact match, the corresponding path is marked hit.
 */
typedef struct _pcov_frame_t {
	zend_execute_data *ex;         /* frame identity (VM stack slot)          */
	void              *op_opcodes; /* op_array->opcodes: disambiguates reuse  */
	php_pcov_reached_t *cache;     /* cached analysis for this op_array       */
	uint32_t          *seq;        /* branch-id sequence                      */
	uint32_t           seq_len;
	uint32_t           seq_size;
	int32_t            last;       /* last branch id appended (-1 = none)     */
} pcov_frame_t;

typedef struct _pcov_frame_stack_t {
	pcov_frame_t *frames;
	uint32_t      count;
	uint32_t      size;
} pcov_frame_stack_t;

static pcov_frame_stack_t *php_pcov_frames(void) {
	pcov_frame_stack_t *s = (pcov_frame_stack_t *) PCG(frames);
	if (!s) {
		s = ecalloc(1, sizeof(pcov_frame_stack_t));
		PCG(frames) = s;
	}
	return s;
}

/* Match a completed frame's branch sequence against enumerated paths. */
static void php_pcov_frame_finalize(pcov_frame_t *f) {
	if (f->cache && f->cache->info && f->cache->path_hits && f->seq_len) {
		pcov_branch_mark_path_hit(f->cache->info, f->cache->path_hits, f->seq, f->seq_len);
	}
	/* The invocation ended at its last recorded branch; if that branch has an
	 * EXIT out-edge, that exit was actually taken. Record it so out_hit for
	 * exit edges reflects a real traversal rather than mere branch reachability
	 * (matters for ZEND_LAST_CATCH, which has both a fall-through and an EXIT
	 * successor). */
	if (f->cache && f->cache->info && f->cache->edges && f->seq_len) {
		uint32_t term = f->seq[f->seq_len - 1];
		if (term < f->cache->info->size) {
			pcov_branch *pb = &f->cache->info->branches[term];
			uint32_t j;
			for (j = 0; j < pb->outs_count; j++) {
				if (pb->outs[j] == PCOV_JMP_EXIT) {
					uint32_t ei = pcov_branch_edge_index(f->cache->info, term, j);
					if (ei < f->cache->edges_bits) {
						zend_bitset_incl(f->cache->edges, ei);
					}
				}
			}
		}
	}
	if (f->seq) {
		efree(f->seq);
		f->seq = NULL;
	}
	f->seq_len = f->seq_size = 0;
	f->ex = NULL;
	f->op_opcodes = NULL;
	f->cache = NULL;
	f->last = -1;
}

static void php_pcov_frames_dtor(void) {
	pcov_frame_stack_t *s = (pcov_frame_stack_t *) PCG(frames);
	uint32_t i;
	if (!s) {
		return;
	}
	for (i = 0; i < s->count; i++) {
		php_pcov_frame_finalize(&s->frames[i]);
	}
	if (s->frames) {
		efree(s->frames);
	}
	efree(s);
	PCG(frames) = NULL;
}

static void php_pcov_frames_reset(void) {
	pcov_frame_stack_t *s = (pcov_frame_stack_t *) PCG(frames);
	uint32_t i;
	if (!s) {
		return;
	}
	for (i = 0; i < s->count; i++) {
		if (s->frames[i].seq) {
			efree(s->frames[i].seq);
			s->frames[i].seq = NULL;
		}
	}
	s->count = 0;
}

/* Is this frame still live on the current VM call stack? Matches on BOTH the
 * execute_data pointer AND the op_array identity: VM stack slots are reused, so
 * a returned frame's `ex` can coincide with a live frame running a different
 * op_array. Requiring the opcodes pointer to match too avoids treating such a
 * returned frame as still live (which would leave its final path unfinalized). */
static int php_pcov_frame_is_live(zend_execute_data *ex, void *op_opcodes) {
	zend_execute_data *cur = EG(current_execute_data);
	while (cur) {
		if (cur == ex && cur->func &&
		    (void *) cur->func->op_array.opcodes == op_opcodes) {
			return 1;
		}
		cur = cur->prev_execute_data;
	}
	return 0;
}

/* Finalize and drop only frames whose functions have already returned (are no
 * longer on the live VM stack), preserving still-active frames. Used by stop()
 * and collect() so a collect() taken mid-execution does not split a live
 * function's path into unmatchable prefix/suffix sequences. */
static void php_pcov_frames_finalize_returned(void) {
	pcov_frame_stack_t *s = (pcov_frame_stack_t *) PCG(frames);
	uint32_t i, w = 0;
	if (!s) {
		return;
	}
	for (i = 0; i < s->count; i++) {
		if (php_pcov_frame_is_live(s->frames[i].ex, s->frames[i].op_opcodes)) {
			/* keep: compact toward the front, preserving order */
			if (w != i) {
				s->frames[w] = s->frames[i];
			}
			w++;
		} else {
			php_pcov_frame_finalize(&s->frames[i]);
		}
	}
	s->count = w;
}

/* Finalize and remove every frame whose cached analysis is `r`. Called just
 * before a stale reached-cache entry is freed so no pcov_frame_t retains a
 * dangling `cache` pointer (which php_pcov_frame_finalize would later
 * dereference). Finalizing here still records the frame's path against the
 * (about-to-be-freed) entry, which is harmless — the entry is being discarded
 * as stale anyway. */
static void php_pcov_frames_detach_cache(php_pcov_reached_t *r) {
	pcov_frame_stack_t *s = (pcov_frame_stack_t *) PCG(frames);
	uint32_t i, w = 0;
	if (!s || !r) {
		return;
	}
	for (i = 0; i < s->count; i++) {
		if (s->frames[i].cache == r) {
			php_pcov_frame_finalize(&s->frames[i]); /* frees seq, clears cache */
		} else {
			if (w != i) {
				s->frames[w] = s->frames[i];
			}
			w++;
		}
	}
	s->count = w;
}

static void php_pcov_frame_append(pcov_frame_t *f, uint32_t branch_id) {
	/* Dedup: xdebug records a branch start only the first consecutive time. */
	if (f->last == (int32_t) branch_id) {
		return;
	}
	if (f->seq_len == f->seq_size) {
		f->seq_size += 32;
		f->seq = erealloc(f->seq, sizeof(uint32_t) * f->seq_size);
	}
	f->seq[f->seq_len++] = branch_id;
	f->last = (int32_t) branch_id;
}

/*
 * Reconcile the live PHP call stack with our tracked frame stack, then record
 * the current opcode if it is a branch start.
 *
 * We use the machine-stack ordering of zend_execute_data pointers: a deeper
 * (more recently entered) frame lives at a lower address than its caller on
 * the VM stack, but rather than rely on address direction we track by exact
 * pointer identity: when EX changes to a pointer we already have on the stack
 * we pop everything above it (those frames returned); when it is new we push.
 */
static zend_always_inline void php_pcov_path_trace(zend_execute_data *execute_data, zend_op_array *op_array) {
	pcov_frame_stack_t *s = php_pcov_frames();
	pcov_frame_t *top;
	php_pcov_reached_t *cache;
	uint32_t idx;
	int found_at = -1;
	uint32_t i;

	/* Is this execute_data already a tracked frame? A frame matches only when
	 * BOTH the execute_data pointer AND the op_array identity match: VM stack
	 * slots are reused across calls, so the same ex pointer with a different
	 * op_array means the previous occupant returned. */
	for (i = s->count; i > 0; i--) {
		if (s->frames[i - 1].ex == execute_data &&
		    s->frames[i - 1].op_opcodes == (void *) op_array->opcodes) {
			found_at = (int) (i - 1);
			break;
		}
	}

	if (found_at >= 0) {
		/* Frames above found_at have returned: finalize and pop them. */
		while (s->count > (uint32_t) found_at + 1) {
			php_pcov_frame_finalize(&s->frames[s->count - 1]);
			s->count--;
		}
		top = &s->frames[found_at];
	} else {
		/* New frame. Its op_array must be analyzable to track paths. */
		cache = php_pcov_reached_get(op_array);
		if (!cache) {
			return;
		}
		if (s->count == s->size) {
			s->size += 16;
			s->frames = erealloc(s->frames, sizeof(pcov_frame_t) * s->size);
		}
		top = &s->frames[s->count++];
		top->ex = execute_data;
		top->op_opcodes = (void *) op_array->opcodes;
		top->cache = cache;
		top->seq = NULL;
		top->seq_len = top->seq_size = 0;
		top->last = -1;
	}

	/* Record branch start + branch-hit for the current opcode. */
	cache = top->cache;
	if (!cache || !cache->info) {
		return;
	}
	idx = (uint32_t) (execute_data->opline - op_array->opcodes);

	/* Re-entry detection: the VM reuses stack slots, so a fresh invocation of
	 * the same function lands on the same (ex, opcodes) frame. When we observe
	 * an entry point again after having already recorded a sequence, the
	 * previous invocation ended: finalize its path, then start a new one. This
	 * is the pcov analogue of xdebug's entry_point re-entry handling. */
	if (top->seq_len > 0 && idx < cache->info->size &&
	    zend_bitset_in(cache->info->entry_points, idx)) {
		php_pcov_frame_finalize(top);
		/* finalize cleared identity fields; restore this live frame */
		top->ex = execute_data;
		top->op_opcodes = (void *) op_array->opcodes;
		top->cache = cache;
	}

	if (pcov_branch_is_start(cache->info, idx)) {
		if (idx < cache->reached_bits) {
			zend_bitset_incl(cache->reached, idx);
		}
		/* Record the actually-traversed edge (prev_branch -> idx) rather than
		 * inferring it from cumulative reachability. Find the out-index of the
		 * previous branch whose target is idx and mark that edge. */
		if (top->last >= 0 && cache->edges) {
			pcov_branch *pb = &cache->info->branches[top->last];
			uint32_t j;
			for (j = 0; j < pb->outs_count; j++) {
				if (pb->outs[j] == (int) idx) {
					uint32_t ei = pcov_branch_edge_index(cache->info, (uint32_t) top->last, j);
					if (ei < cache->edges_bits) {
						zend_bitset_incl(cache->edges, ei);
					}
				}
			}
		}
		php_pcov_frame_append(top, idx);
	}
}

/* Forward decls for functions used by the xdebug-compat shim. */
static void php_pcov_collect_common(zval *return_value, zend_long type, zval *filter);
static zend_always_inline void php_pcov_clean(HashTable *table);
static zend_always_inline zend_bool php_pcov_filter_admits(zend_string *filename);

/* Whether branch mode is requested. Usable at MINIT (env or ini string),
 * before RINIT resolves PCG(mode). */
int php_pcov_branch_mode_requested(void) {
	return php_pcov_resolve_mode() == PCOV_MODE_BRANCH;
}

/* Non-static wrapper so the xdebug-compat shim can gate registration on the
 * same enabled state RINIT uses (pcov.enabled / PCOV_ENABLED). */
int php_pcov_is_api_enabled(void) {
	return php_pcov_api_enabled() ? 1 : 0;
}

void php_pcov_start_internal(void) {
	if (!php_pcov_api_enabled()) {
		return;
	}
	PCG(enabled) = 1;
}

/* Reset accumulated coverage state. Mirrors \pcov\clear(): when files is true
 * the discovered-file tables are dropped too. Declared here and reused by the
 * xdebug-compat stop($cleanup=true) path so both share one reset. */
void php_pcov_clear_internal(int files) {
	if (files) {
		php_pcov_clean(&PCG(files));
		php_pcov_clean(&PCG(discovered));
	}

	zend_arena_destroy(PCG(mem));
	PCG(mem) = zend_arena_create(INI_INT("pcov.initial.memory"));

	PCG(start) = NULL;
	PCG(last)  = NULL;
	PCG(next)  = NULL;

	php_pcov_clean(&PCG(waiting));
	php_pcov_clean(&PCG(covered));
	php_pcov_clean(&PCG(reached));
	php_pcov_frames_reset();
}

void php_pcov_stop_internal(int cleanup) {
	if (!php_pcov_api_enabled()) {
		return;
	}
	PCG(enabled) = 0;

	/* Finalize any frames whose functions have already returned so their
	 * per-invocation path sequences are matched before state may be cleared. */
	php_pcov_frames_finalize_returned();

	/* xdebug_stop_code_coverage($cleanup=true) discards collected data so the
	 * next run starts clean; $cleanup=false preserves it. */
	if (cleanup) {
		php_pcov_clear_internal(0);
	}
}

void php_pcov_collect_into(zval *return_value, zend_long type, zval *filter) {
	php_pcov_collect_common(return_value, type, filter);
}

static PHP_GINIT_FUNCTION(pcov)
{
#if defined(COMPILE_DL_PCOV) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	ZEND_SECURE_ZERO(pcov_globals, sizeof(zend_pcov_globals));
}

/* Apply the xdebug_set_filter() path-prefix filter (case-insensitive, matching
 * Xdebug semantics). Returns 1 if the file is admitted by the current filter,
 * 0 if it is filtered out. When no filter is active, everything is admitted. */
static zend_always_inline zend_bool php_pcov_filter_admits(zend_string *filename) {
	zend_string *prefix;
	zend_bool matched = 0;

	if (PCG(filter_mode) == PCOV_FILTER_MODE_NONE || !PCG(filter_paths_init)) {
		return 1;
	}

	ZEND_HASH_FOREACH_STR_KEY(&PCG(filter_paths), prefix) {
		if (prefix && ZSTR_LEN(filename) >= ZSTR_LEN(prefix) &&
		    zend_binary_strncasecmp(
		        ZSTR_VAL(filename), ZSTR_LEN(filename),
		        ZSTR_VAL(prefix), ZSTR_LEN(prefix),
		        ZSTR_LEN(prefix)) == 0) {
			matched = 1;
			break;
		}
	} ZEND_HASH_FOREACH_END();

	if (PCG(filter_mode) == PCOV_FILTER_MODE_INCLUDE) {
		return matched;      /* include: only listed prefixes */
	}
	return matched ? 0 : 1;  /* exclude: everything but listed prefixes */
}

/* Called by the xdebug-compat xdebug_set_filter(). list_type follows Xdebug:
 * 1 = XDEBUG_PATH_INCLUDE, 0 = XDEBUG_PATH_EXCLUDE, anything else (e.g.
 * XDEBUG_FILTER_NONE = -1) clears the filter. configuration is an array of
 * path-prefix strings. Resets the wants/ignores caches so the new filter is
 * applied consistently. */
void php_pcov_compat_set_filter(zend_long list_type, zval *configuration) {
	zval *entry;

	if (!PCG(filter_paths_init)) {
		zend_hash_init(&PCG(filter_paths), 8, NULL, NULL, 0);
		PCG(filter_paths_init) = 1;
	} else {
		zend_hash_clean(&PCG(filter_paths));
	}

	if (list_type == PCOV_XDEBUG_LIST_PATH_INCLUDE) {
		PCG(filter_mode) = PCOV_FILTER_MODE_INCLUDE;
	} else if (list_type == PCOV_XDEBUG_LIST_PATH_EXCLUDE) {
		PCG(filter_mode) = PCOV_FILTER_MODE_EXCLUDE;
	} else {
		/* XDEBUG_FILTER_NONE (0) or anything unrecognized clears the filter. */
		PCG(filter_mode) = PCOV_FILTER_MODE_NONE;
	}

	if (PCG(filter_mode) != PCOV_FILTER_MODE_NONE && configuration) {
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(configuration), entry) {
			if (Z_TYPE_P(entry) == IS_STRING && Z_STRLEN_P(entry) > 0) {
				/* zend_hash_add_empty_element takes its own reference on the
				 * key; do not pass an extra zend_string_copy or it leaks. */
				zend_hash_add_empty_element(&PCG(filter_paths), Z_STR_P(entry));
			}
		} ZEND_HASH_FOREACH_END();
	}

	/* Filter changed: drop the admission caches so decisions are re-evaluated. */
	php_pcov_clean(&PCG(wants));
	php_pcov_clean(&PCG(ignores));
}

static zend_always_inline zend_bool php_pcov_wants(zend_string *filename) { /* {{{ */
	if (!PCG(directory)) {
		return php_pcov_filter_admits(filename);
	}

	if (ZSTR_LEN(filename) < ZSTR_LEN(PCG(directory))) {
		return 0;
	}

	if (zend_hash_exists(&PCG(wants), filename)) {
		return 1;
	}

	if (zend_hash_exists(&PCG(ignores), filename)) {
		return 0;
	}

	if (!php_pcov_filter_admits(filename)) {
		zend_hash_add_empty_element(&PCG(ignores), filename);
		return 0;
	}

	if (strncmp(
		ZSTR_VAL(filename),
		ZSTR_VAL(PCG(directory)),
		ZSTR_LEN(PCG(directory))) == SUCCESS) {

		if (PCG(exclude)) {
			zval match;

			ZVAL_UNDEF(&match);

			php_pcre_match_impl(
				PCG(exclude),
#if PHP_VERSION_ID >= 70400
				filename,
#else
				ZSTR_VAL(filename), ZSTR_LEN(filename),
#endif
				&match, NULL,
#if PHP_VERSION_ID >= 80400
				false, 0, 0);
#else
				0, 0, 0, 0);
#endif

			if (zend_is_true(&match)) {
				zend_hash_add_empty_element(
					&PCG(ignores), filename);
				return 0;
			}
		}

		zend_hash_add_empty_element(&PCG(wants), filename);
		return 1;
	}

	zend_hash_add_empty_element(&PCG(ignores), filename);
	return 0;
} /* }}} */

static void php_pcov_fill_ignored_opcode_lut(void) {
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_NOP);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_OP_DATA);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_FE_FREE);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_FREE);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_ASSERT_CHECK);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_VERIFY_RETURN_TYPE);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_RECV);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_RECV_INIT);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_RECV_VARIADIC);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_SEND_VAL);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_SEND_VAR_EX);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_SEND_REF);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_SEND_UNPACK);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_DECLARE_CONST);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_DECLARE_CLASS);
#ifdef ZEND_DECLARE_INHERITED_CLASS
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_DECLARE_INHERITED_CLASS);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_DECLARE_INHERITED_CLASS_DELAYED);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_DECLARE_ANON_INHERITED_CLASS);
#else
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_DECLARE_CLASS_DELAYED);
#endif
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_DECLARE_FUNCTION);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_DECLARE_ANON_CLASS);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_FAST_RET);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_FAST_CALL);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_TICKS);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_EXT_STMT);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_EXT_FCALL_BEGIN);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_EXT_FCALL_END);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_EXT_NOP);
#if PHP_VERSION_ID < 70400
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_VERIFY_ABSTRACT_CLASS);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_ADD_TRAIT);
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_BIND_TRAITS);
#endif
	zend_bitset_incl(php_pcov_opcode_lut, ZEND_BIND_GLOBAL);
}

static zend_always_inline zend_bool php_pcov_ignored_opcode(zend_uchar opcode) { /* {{{ */
	return zend_bitset_in(php_pcov_opcode_lut, opcode);
} /* }}} */

static zend_always_inline zend_string* php_pcov_interned_string(zend_string *string) { /* {{{ */
	if (ZSTR_IS_INTERNED(string)) {
		return string;
	}

	return zend_new_interned_string(zend_string_copy(string));
} /* }}} */

static zend_always_inline php_coverage_t* php_pcov_create(zend_execute_data *execute_data) { /* {{{ */
	php_coverage_t *create = (php_coverage_t*) zend_arena_alloc(&PCG(mem), sizeof(php_coverage_t));

	create->file     = php_pcov_interned_string(EX(func)->op_array.filename);
	create->line     = EX(opline)->lineno;
	create->next     = NULL;

	zend_hash_add_empty_element(&PCG(waiting), create->file);

	return create;
} /* }}} */

static zend_always_inline int php_pcov_has(zend_string *filename, uint32_t lineno) { /* {{{ */
	HashTable *table = zend_hash_find_ptr(&PCG(covered), filename);

	if (UNEXPECTED(!table)) {
		HashTable covering;

		zend_hash_init(&covering, 64, NULL, NULL, 0);

		table = zend_hash_add_mem(
			&PCG(covered), filename, &covering, sizeof(HashTable));

 		zend_hash_index_add_empty_element(table, lineno);
		return 0;
	}

	if (EXPECTED(zend_hash_index_exists(table, lineno))) {
		return 1;
	}

	zend_hash_index_add_empty_element(table, lineno);
	return 0;
} /* }}} */

static zend_always_inline int php_pcov_trace(zend_execute_data *execute_data) { /* {{{ */
    if (PCG(enabled)) {
		zend_op_array *op_array = &EX(func)->op_array;

		if (php_pcov_wants(op_array->filename)) {
			/* Branch mode: record branch-start entry + per-invocation path
			 * sequence for this frame. */
			if (PCG(mode) == PCOV_MODE_BRANCH && op_array->type == ZEND_USER_FUNCTION) {
				php_pcov_path_trace(execute_data, op_array);
			}

			if (!php_pcov_ignored_opcode(EX(opline)->opcode) &&
				!php_pcov_has(op_array->filename, EX(opline)->lineno)) {

				php_coverage_t *coverage = php_pcov_create(execute_data);

				if (!PCG(start)) {
					PCG(start) = coverage;
				} else {
					*(PCG(next)) = coverage;
				}

				PCG(next) = &coverage->next;
			}
		}
	}

	return zend_vm_call_opcode_handler(execute_data);
} /* }}} */

zend_op_array* php_pcov_compile_file(zend_file_handle *fh, int type) { /* {{{ */
	zend_op_array *result = zend_compile_file_function(fh, type), *mem;

	if (!result || !result->filename || !php_pcov_wants(result->filename)) {
		return result;
	}

	if (zend_hash_exists(&PCG(files), result->filename)) {
		return result;
	}

	mem = zend_hash_add_mem(
			&PCG(files),
			result->filename,
			result, sizeof(zend_op_array));

#if PHP_VERSION_ID >= 70400
	if (result->refcount) {
		(*result->refcount)++;
	}
	if (result->static_variables) {
		if (!(GC_FLAGS(result->static_variables) & IS_ARRAY_IMMUTABLE)) {
			GC_ADDREF(result->static_variables);
		}
	}
	mem->fn_flags &= ~ZEND_ACC_HEAP_RT_CACHE;
#else
	(void)mem;
	function_add_ref((zend_function*)result);
#endif

	return result;
} /* }}} */

void php_pcov_execute_ex(zend_execute_data *execute_data) { /* {{{ */
	int zrc		= 0;

	while (1) {
		zrc = php_pcov_trace(execute_data);

		if (zrc != SUCCESS) {
			if (zrc < SUCCESS) {
				return;
			}
			execute_data = EG(current_execute_data);
		}
	}
} /* }}} */

void php_pcov_covered_dtor(zval *zv) { /* {{{ */
	zend_hash_destroy(Z_PTR_P(zv));
	efree(Z_PTR_P(zv));
} /* }}} */

void php_pcov_files_dtor(zval *zv) { /* {{{ */
	destroy_op_array(Z_PTR_P(zv));
	efree(Z_PTR_P(zv));
} /* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(pcov)
{
	REGISTER_NS_LONG_CONSTANT("pcov", "all",         PCOV_FILTER_ALL,     CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("pcov", "inclusive",   PCOV_FILTER_INCLUDE, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("pcov", "exclusive",   PCOV_FILTER_EXCLUDE, CONST_CS|CONST_PERSISTENT);

	REGISTER_NS_STRING_CONSTANT("pcov", "version",     PHP_PCOV_VERSION,    CONST_CS|CONST_PERSISTENT);

	REGISTER_INI_ENTRIES();

	/* Route (c): opt-in xdebug-compatible surface so unmodified
	 * php-code-coverage / PHPUnit can drive pcov for --path-coverage.
	 * No-op unless pcov.mode=branch and the real Xdebug is absent. */
	php_pcov_xdebug_compat_minit(INIT_FUNC_ARGS_PASSTHRU);

	if (php_pcov_api_enabled()) {
		zend_execute_ex_function   = zend_execute_ex;
		zend_execute_ex            = php_pcov_execute_ex;
	}

	ZVAL_LONG(&php_pcov_uncovered,   PHP_PCOV_UNCOVERED);
	ZVAL_LONG(&php_pcov_covered,     PHP_PCOV_COVERED);

	php_pcov_fill_ignored_opcode_lut();

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(pcov)
{
	if (php_pcov_api_enabled()) {
		zend_execute_ex   = zend_execute_ex_function;
	}

	UNREGISTER_INI_ENTRIES();

	return SUCCESS;
}
/* }}} */

const char *php_pcov_directory_defaults[] = { /* {{{ */
	"src",
	"lib",
	"app",
	".",
	NULL
}; /* }}} */

static  void php_pcov_setup_directory(char *directory) { /* {{{ */
	char        realpath[MAXPATHLEN];
	zend_stat_t statbuf;

	if (!directory || !*directory) {
		const char** try = php_pcov_directory_defaults;

		while (*try) {
			if (VCWD_REALPATH(*try, realpath) &&
			    VCWD_STAT(realpath, &statbuf) == SUCCESS) {
				directory = realpath;
				break;
			}
			try++;
		}
	} else {
		if (VCWD_REALPATH(directory, realpath) &&
		    VCWD_STAT(realpath, &statbuf) == SUCCESS) {
			directory = realpath;
		}
	}

	PCG(directory) = zend_string_init(directory, strlen(directory), 0);
} /* }}} */

static zend_always_inline void php_pcov_setup_exclude(char *exclude) { /* {{{ */
	zend_string *pattern;

	if (!exclude || !*exclude) {
		return;
	}

	pattern = zend_string_init(
		exclude, strlen(exclude), 0);

	PCG(exclude) = pcre_get_compiled_regex_cache(pattern);

	if (PCG(exclude)) {
		php_pcre_pce_incref(PCG(exclude));
	}

	zend_string_release(pattern);
} /* }}} */

/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(pcov)
{
#if defined(COMPILE_DL_PCOV) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	if (!php_pcov_api_enabled()) {
		return SUCCESS;
	}

	PCG(mem) = zend_arena_create(INI_INT("pcov.initial.memory"));

	zend_hash_init(&PCG(files),      INI_INT("pcov.initial.files"), NULL, php_pcov_files_dtor, 0);
	zend_hash_init(&PCG(waiting),    INI_INT("pcov.initial.files"), NULL, NULL, 0);
	zend_hash_init(&PCG(ignores),    INI_INT("pcov.initial.files"), NULL, NULL, 0);
	zend_hash_init(&PCG(wants),      INI_INT("pcov.initial.files"), NULL, NULL, 0);
	zend_hash_init(&PCG(discovered), INI_INT("pcov.initial.files"), NULL, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&PCG(covered),    INI_INT("pcov.initial.files"), NULL, php_pcov_covered_dtor, 0);
	zend_hash_init(&PCG(reached),    INI_INT("pcov.initial.files"), NULL, php_pcov_reached_dtor, 0);

	PCG(mode) = php_pcov_resolve_mode();
	PCG(frames) = NULL;
	PCG(filter_mode) = PCOV_FILTER_MODE_NONE;
	PCG(filter_paths_init) = 0;

	php_pcov_setup_directory(INI_STR("pcov.directory"));
	php_pcov_setup_exclude(INI_STR("pcov.exclude"));

#ifdef ZEND_COMPILE_NO_JUMPTABLES
	CG(compiler_options) |= ZEND_COMPILE_NO_JUMPTABLES;
#endif

	if  (!zend_compile_file_function) {
		zend_compile_file_function = zend_compile_file;
		zend_compile_file          = php_pcov_compile_file;
	}

	PCG(start) = NULL;
	PCG(last)  = NULL;
	PCG(next)  = NULL;

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(pcov)
{
	if (!php_pcov_api_enabled() || CG(unclean_shutdown)) {
		return SUCCESS;
	}

	zend_hash_destroy(&PCG(files));
	zend_hash_destroy(&PCG(ignores));
	zend_hash_destroy(&PCG(wants));
	zend_hash_destroy(&PCG(discovered));
	php_pcov_frames_dtor();

	zend_hash_destroy(&PCG(waiting));
	zend_hash_destroy(&PCG(covered));
	zend_hash_destroy(&PCG(reached));

	if (PCG(filter_paths_init)) {
		zend_hash_destroy(&PCG(filter_paths));
		PCG(filter_paths_init) = 0;
	}

	zend_arena_destroy(PCG(mem));

	if (PCG(directory)) {
		zend_string_release(PCG(directory));
	}

	if (PCG(exclude)) {
		php_pcre_pce_decref(PCG(exclude));
	}

	if (zend_compile_file == php_pcov_compile_file) {
		zend_compile_file = zend_compile_file_function;
		zend_compile_file_function = NULL;
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(pcov)
{
	char info[64];
	char *directory = INI_STR("pcov.directory");
	char *exclude   = INI_STR("pcov.exclude");

	php_info_print_table_start();

	php_info_print_table_header(2,
		"PCOV support",
		php_pcov_api_enabled()  ? "Enabled" : "Disabled");
	php_info_print_table_row(2,
		"PCOV version",
		PHP_PCOV_VERSION);
	php_info_print_table_row(2,
		"pcov.directory",
		directory && *directory ? directory : (PCG(directory) ? ZSTR_VAL(PCG(directory)) : "auto"));
	php_info_print_table_row(2,
		"pcov.exclude",
		exclude   && *exclude   ? exclude : "none" );

	snprintf(info, sizeof(info),
		ZEND_LONG_FMT " bytes",
		(zend_long) INI_INT("pcov.initial.memory"));
	php_info_print_table_row(2,
		"pcov.initial.memory", info);

	snprintf(info, sizeof(info),
		ZEND_LONG_FMT,
		(zend_long) INI_INT("pcov.initial.files"));
	php_info_print_table_row(2,
		"pcov.initial.files", info);

	php_info_print_table_end();
}
/* }}} */

static zend_always_inline void php_pcov_report(php_coverage_t *coverage, zval *filter) { /* {{{ */
	zval *table;
	zval *hit;

	if (!coverage) {
		return;
	}

	do {
		if ((table = zend_hash_find(Z_ARRVAL_P(filter), coverage->file))) {
			if ((hit = zend_hash_index_find(Z_ARRVAL_P(table), coverage->line))) {
				Z_LVAL_P(hit) = PHP_PCOV_COVERED;
			}
		}
	} while ((coverage = coverage->next));
} /* }}} */

/* Branch-mode line report: same as php_pcov_report but the per-file line map
 * lives under return_value[file]['lines']. */
static zend_always_inline void php_pcov_report_branch(php_coverage_t *coverage, zval *filter) { /* {{{ */
	zval *file_info;
	zval *lines;
	zval *hit;

	if (!coverage) {
		return;
	}

	do {
		if ((file_info = zend_hash_find(Z_ARRVAL_P(filter), coverage->file)) &&
		    Z_TYPE_P(file_info) == IS_ARRAY &&
		    (lines = zend_hash_str_find(Z_ARRVAL_P(file_info), "lines", sizeof("lines") - 1))) {
			if ((hit = zend_hash_index_find(Z_ARRVAL_P(lines), coverage->line))) {
				Z_LVAL_P(hit) = PHP_PCOV_COVERED;
			}
		}
	} while ((coverage = coverage->next));
} /* }}} */

static void php_pcov_discover_code(zend_arena **arena, zend_op_array *ops, zval *return_value) { /* {{{ */
	zend_cfg cfg;
	zend_basic_block *block;
	zend_op *limit = ops->opcodes + ops->last;
	int i = 0;

	if (ops->fn_flags & ZEND_ACC_ABSTRACT) {
		return;
	}

	memset(&cfg, 0, sizeof(zend_cfg));

	zend_build_cfg(arena, ops,  PHP_PCOV_CFG, &cfg);

	for (block = cfg.blocks, i = 0; i < cfg.blocks_count; i++, block++) {
		zend_op *opline = ops->opcodes + block->start,
			*end = opline + block->len;

		if (!(block->flags & ZEND_BB_REACHABLE)) {
			/*
			* Note that, we don't care about unreachable blocks
			* that would be removed by opcache, because it would
			* create different reports depending on configuration
			*/
			continue;
		}

		while(opline < end) {
			if (php_pcov_ignored_opcode(opline->opcode)) {
				opline++;
				continue;
			}

			if (!zend_hash_index_exists(Z_ARRVAL_P(return_value), opline->lineno)) {
				zend_hash_index_add(
					Z_ARRVAL_P(return_value),
					opline->lineno, &php_pcov_uncovered);
			}

			if ((opline +0)->opcode == ZEND_NEW &&
			    (opline +1)->opcode == ZEND_DO_FCALL) {
				opline++;
			}

			opline++;
		}

		if (block == cfg.blocks && opline == limit) {
			/*
			* If the first basic block finishes at the end of the op array
			* then we don't care about subsequent blocks
			*/
			break;
		}
	}

#if PHP_VERSION_ID >= 80100
    for (uint32_t def = 0; def < ops->num_dynamic_func_defs; def++) {
        php_pcov_discover_code(arena, ops->dynamic_func_defs[def], return_value);
    }
#endif
} /* }}} */

static void php_pcov_discover_file(zend_string *file, zval *return_value) { /* {{{ */
	zval discovered;
	zend_op_array *ops;
	zval *cache;
	zend_arena *mem;

	/* Honor an xdebug_set_filter() path filter even for files already admitted
	 * into PCG(files) before the filter was set. */
	if (!php_pcov_filter_admits(file)) {
		return;
	}

	cache = zend_hash_find(&PCG(discovered), file);

	if (cache) {
		zval uncached;
		ZVAL_DUP(&uncached, cache);

		zend_hash_update(Z_ARRVAL_P(return_value), file, &uncached);
		return;
	}

	if (!(ops = zend_hash_find_ptr(&PCG(files), file))) {
		return;
	}

	array_init(&discovered);

	mem = zend_arena_create(1024 * 1024);

	php_pcov_discover_code(&mem, ops, &discovered);
	{
		zend_class_entry *ce;
		zend_op_array    *function;
		ZEND_HASH_FOREACH_PTR(EG(class_table), ce) {
			if (ce->type != ZEND_USER_CLASS) {
				continue;
			}

			ZEND_HASH_FOREACH_PTR(&ce->function_table, function) {
				if (function->type == ZEND_USER_FUNCTION &&
				    function->filename &&
				    zend_string_equals(file, function->filename)) {
					php_pcov_discover_code(&mem, function, &discovered);
				}
			} ZEND_HASH_FOREACH_END();

#if PHP_VERSION_ID >= 80400
			if (ce->num_hooked_props > 0) {
				zend_property_info *prop;
				ZEND_HASH_MAP_FOREACH_PTR(&ce->properties_info, prop) {
					if (prop->hooks) {
						for (uint32_t i = 0; i < ZEND_PROPERTY_HOOK_COUNT; i++) {
							if (prop->hooks[i]) {
								function = &prop->hooks[i]->op_array;
								if (function->type == ZEND_USER_FUNCTION &&
									function->filename &&
									zend_string_equals(file, function->filename)) {
									php_pcov_discover_code(&mem, function, &discovered);
								}
							}
						}
					}
				} ZEND_HASH_FOREACH_END();
			}
#endif
		} ZEND_HASH_FOREACH_END();
	}

	{
		zend_op_array *function;
		ZEND_HASH_FOREACH_PTR(EG(function_table), function) {
			if (function->type == ZEND_USER_FUNCTION &&
			    function->filename &&
			    zend_string_equals(file, function->filename)) {
				php_pcov_discover_code(&mem, function, &discovered);
			}
		} ZEND_HASH_FOREACH_END();
	}

	zend_hash_update(&PCG(discovered), file, &discovered);
	zend_arena_destroy(mem);
	
	php_pcov_discover_file(file, return_value);
} /* }}} */

static zend_always_inline void php_pcov_clean(HashTable *table) { /* {{{ */
	if (table->nNumUsed) {
		zend_hash_clean(table);
	}
} /* }}} */

/* Build branch+path info for one op_array and add it (keyed by its canonical
 * function name) to the `functions` array. Recurses into dynamic func defs. */
static void php_pcov_discover_functions_branch(zend_op_array *ops, zval *functions) { /* {{{ */
	php_pcov_reached_t *cache;
	pcov_branch_info *info;
	char key[1024];
	zval z_function;

	if (ops->fn_flags & ZEND_ACC_ABSTRACT) {
		return;
	}

	/* Get-or-build the cached analysis. php_pcov_reached_get() stores the CFG
	 * (with empty hit sets for never-executed functions) in PCG(reached), so a
	 * function is analyzed once per request even though php_pcov_discover_file_branch()
	 * runs on every collect() and walks the class/function tables each time.
	 * For a covered function the same entry already carries reached/path/edges. */
	cache = php_pcov_reached_get(ops);
	if (!cache || !cache->info) {
		return;
	}
	info = cache->info;

	pcov_branch_function_key(key, sizeof(key), ops);

	array_init(&z_function);
	pcov_branch_info_to_zval_ex(
		&z_function, info,
		cache->reached, cache->reached_bits,
		cache->path_hits, cache->path_hits_bits,
		cache->edges, cache->edges_bits);

	/* Later definitions of the same key (e.g. re-included files) overwrite;
	 * matches how xdebug's hash-keyed storage behaves. */
	add_assoc_zval_ex(functions, key, strlen(key), &z_function);

#if PHP_VERSION_ID >= 80100
	{
		uint32_t def;
		for (def = 0; def < ops->num_dynamic_func_defs; def++) {
			php_pcov_discover_functions_branch(ops->dynamic_func_defs[def], functions);
		}
	}
#endif
} /* }}} */

/* Branch-mode analogue of php_pcov_discover_file: emit { lines, functions }
 * for one file. `lines` reuses the existing line discovery + report. */
static void php_pcov_discover_file_branch(zend_string *file, zval *return_value, php_coverage_t *line_hits) { /* {{{ */
	zend_op_array *ops;
	zval file_info, lines, functions;

	/* Honor an xdebug_set_filter() path filter (see php_pcov_discover_file). */
	if (!php_pcov_filter_admits(file)) {
		return;
	}

	if (!(ops = zend_hash_find_ptr(&PCG(files), file))) {
		return;
	}

	array_init(&file_info);

	/* lines: reuse the proven line-discovery path into a temporary array,
	 * then flip hit lines via the existing report walk. */
	array_init(&lines);
	{
		zval tmp;
		zval *entry;
		zend_ulong lineno;
		/* discover executable lines for this file into `lines` */
		array_init(&tmp);
		php_pcov_discover_file(file, &tmp);
		if ((entry = zend_hash_find(Z_ARRVAL(tmp), file))) {
			ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(entry), lineno, entry) {
				add_index_long(&lines, lineno, Z_LVAL_P(entry));
			} ZEND_HASH_FOREACH_END();
		}
		zval_ptr_dtor(&tmp);
	}

	/* functions: main op_array + all user functions/methods for this file. */
	array_init(&functions);

	php_pcov_discover_functions_branch(ops, &functions);

	{
		zend_class_entry *ce;
		zend_op_array    *function;
		ZEND_HASH_FOREACH_PTR(EG(class_table), ce) {
			if (ce->type != ZEND_USER_CLASS) {
				continue;
			}
			ZEND_HASH_FOREACH_PTR(&ce->function_table, function) {
				if (function->type == ZEND_USER_FUNCTION &&
				    function->filename &&
				    zend_string_equals(file, function->filename)) {
					php_pcov_discover_functions_branch(function, &functions);
				}
			} ZEND_HASH_FOREACH_END();
#if PHP_VERSION_ID >= 80400
			if (ce->num_hooked_props > 0) {
				zend_property_info *prop;
				ZEND_HASH_MAP_FOREACH_PTR(&ce->properties_info, prop) {
					if (prop->hooks) {
						uint32_t hi;
						for (hi = 0; hi < ZEND_PROPERTY_HOOK_COUNT; hi++) {
							if (prop->hooks[hi]) {
								function = &prop->hooks[hi]->op_array;
								if (function->type == ZEND_USER_FUNCTION &&
									function->filename &&
									zend_string_equals(file, function->filename)) {
									php_pcov_discover_functions_branch(function, &functions);
								}
							}
						}
					}
				} ZEND_HASH_FOREACH_END();
			}
#endif
		} ZEND_HASH_FOREACH_END();
	}

	{
		zend_op_array *function;
		ZEND_HASH_FOREACH_PTR(EG(function_table), function) {
			if (function->type == ZEND_USER_FUNCTION &&
			    function->filename &&
			    zend_string_equals(file, function->filename)) {
				php_pcov_discover_functions_branch(function, &functions);
			}
		} ZEND_HASH_FOREACH_END();
	}

	add_assoc_zval_ex(&file_info, "lines",     sizeof("lines") - 1,     &lines);
	add_assoc_zval_ex(&file_info, "functions", sizeof("functions") - 1, &functions);

	zend_hash_update(Z_ARRVAL_P(return_value), file, &file_info);

	(void) line_hits;
} /* }}} */

/* {{{ array \pcov\collect(int $type = \pcov\all, array $filter = []); */
static void php_pcov_collect_common(zval *return_value, zend_long type, zval *filter)
{
	zval empty_filter;

	array_init(return_value);

	/* Line mode may short-circuit when no new line was hit since the last
	 * collect(). Branch mode must NOT: branch/path hit state (and enumerated
	 * paths from newly returned frames) can change without a new line hit, and
	 * the xdebug-compat API delegates here — a second xdebug_get_code_coverage()
	 * must still return the accumulated data rather than an empty array. */
	if (PCG(mode) != PCOV_MODE_BRANCH) {
		if (PCG(last) == PCG(next)) {
			return;
		}
		PCG(last) = PCG(next);
	} else {
		PCG(last) = PCG(next);
	}

	/* Normalize a missing filter (e.g. \pcov\collect($type) with no array, or
	 * the xdebug-compat surface) to an empty array so the INCLUDE/EXCLUDE paths
	 * never dereference NULL via Z_ARRVAL_P(filter). Freed before returning. */
	if (filter == NULL || Z_TYPE_P(filter) != IS_ARRAY) {
		array_init(&empty_filter);
		filter = &empty_filter;
	} else {
		ZVAL_UNDEF(&empty_filter);
	}

	if (PCG(mode) == PCOV_MODE_BRANCH) {
		/* Finalize only frames whose functions have already returned; leaving
		 * still-active frames intact so a collect() taken mid-execution does
		 * not split a live function's path into unmatchable prefix/suffix. */
		php_pcov_frames_finalize_returned();

		switch (type) {
			case PCOV_FILTER_INCLUDE: {
				zval *filtered;
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(filter), filtered) {
					if (Z_TYPE_P(filtered) != IS_STRING) {
						continue;
					}
					php_pcov_discover_file_branch(Z_STR_P(filtered), return_value, PCG(start));
				} ZEND_HASH_FOREACH_END();
			} break;

			case PCOV_FILTER_EXCLUDE: {
				zend_string *name;
				zval *filtered;
				ZEND_HASH_FOREACH_STR_KEY(&PCG(files), name) {
					ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(filter), filtered) {
						if (Z_TYPE_P(filtered) != IS_STRING) {
							continue;
						}
						if (zend_string_equals(name, Z_STR_P(filtered))) {
							goto _php_pcov_collect_exclude_branch;
						}
					} ZEND_HASH_FOREACH_END();
					php_pcov_discover_file_branch(name, return_value, PCG(start));
				_php_pcov_collect_exclude_branch:
					continue;
				} ZEND_HASH_FOREACH_END();
			} break;

			case PCOV_FILTER_ALL: {
				zend_string *name;
				ZEND_HASH_FOREACH_STR_KEY(&PCG(files), name) {
					php_pcov_discover_file_branch(name, return_value, PCG(start));
				} ZEND_HASH_FOREACH_END();
			} break;
		}

		php_pcov_report_branch(PCG(start), return_value);
		if (Z_TYPE(empty_filter) == IS_ARRAY) {
			zval_ptr_dtor(&empty_filter);
		}
		return;
	}

	switch(type) {
		case PCOV_FILTER_INCLUDE: {
			zval *filtered;
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(filter), filtered) {
				if (Z_TYPE_P(filtered) != IS_STRING) {
					continue;
				}

				php_pcov_discover_file(Z_STR_P(filtered), return_value);
			} ZEND_HASH_FOREACH_END();
		} break;

		case PCOV_FILTER_EXCLUDE: {
			zend_string *name;
			zval *filtered;
			ZEND_HASH_FOREACH_STR_KEY(&PCG(files), name) {
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(filter), filtered) {
					if (Z_TYPE_P(filtered) != IS_STRING) {
						continue;
					}

					if (zend_string_equals(name, Z_STR_P(filtered))) {
						goto _php_pcov_collect_exclude;
					}
				} ZEND_HASH_FOREACH_END();				
				php_pcov_discover_file(name, return_value);

			_php_pcov_collect_exclude:
				continue;
			} ZEND_HASH_FOREACH_END();
		} break;

		case PCOV_FILTER_ALL: {
			zend_string *name;
			ZEND_HASH_FOREACH_STR_KEY(&PCG(files), name) {
				php_pcov_discover_file(name, return_value);
			} ZEND_HASH_FOREACH_END();
		} break;
	}

	php_pcov_report(PCG(start), return_value);

	if (Z_TYPE(empty_filter) == IS_ARRAY) {
		zval_ptr_dtor(&empty_filter);
	}
} /* }}} */

/* {{{ array \pcov\collect(int $type = \pcov\all, array $filter = []); */
PHP_NAMED_FUNCTION(php_pcov_collect)
{
	zend_long type = PCOV_FILTER_ALL;
	zval      *filter = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|la", &type, &filter) != SUCCESS) {
		return;
	}

	PHP_PCOV_API_ENABLED_GUARD();

	if (PCOV_FILTER_ALL != type &&
	    PCOV_FILTER_INCLUDE != type &&
	    PCOV_FILTER_EXCLUDE != type) {
		zend_throw_error(zend_ce_type_error,
			"type must be "
				"\\pcov\\inclusive, "
				"\\pcov\\exclusive, or \\pcov\\all");
		return;
	}

	php_pcov_collect_common(return_value, type, filter);
} /* }}} */

/* {{{ void \pcov\start(void) */
PHP_NAMED_FUNCTION(php_pcov_start)
{
	if (zend_parse_parameters_none() != SUCCESS) {
		return;
	}

	PHP_PCOV_API_ENABLED_GUARD();

	PCG(enabled) = 1;
} /* }}} */

/* {{{ void \pcov\stop(void) */
PHP_NAMED_FUNCTION(php_pcov_stop)
{
	if (zend_parse_parameters_none() != SUCCESS) {
		return;
	}

	PHP_PCOV_API_ENABLED_GUARD();

	PCG(enabled) = 0;

	/* Finalize frames whose functions have already returned so their
	 * per-invocation path sequences are recorded at stop time. */
	if (PCG(mode) == PCOV_MODE_BRANCH) {
		php_pcov_frames_finalize_returned();
	}
} /* }}} */

/* {{{ void \pcov\clear(bool $files = 0) */
PHP_NAMED_FUNCTION(php_pcov_clear)
{
	zend_bool files = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|b", &files) != SUCCESS) {
		return;
	}

	PHP_PCOV_API_ENABLED_GUARD();

	php_pcov_clear_internal(files);
} /* }}} */

/* {{{ array \pcov\waiting(void) */
PHP_NAMED_FUNCTION(php_pcov_waiting)
{
	zend_string *waiting;

	if (zend_parse_parameters_none() != SUCCESS) {
		return;	
	}

	PHP_PCOV_API_ENABLED_GUARD();

	array_init(return_value);

	ZEND_HASH_FOREACH_STR_KEY(&PCG(waiting), waiting) {
		add_next_index_str(
			return_value,
			zend_string_copy(waiting));
	} ZEND_HASH_FOREACH_END();
} /* }}} */

/* {{{ int \pcov\memory(void) */
PHP_NAMED_FUNCTION(php_pcov_memory)
{
	zend_arena *arena = PCG(mem);

	if (zend_parse_parameters_none() != SUCCESS) {
		return;
	}

	PHP_PCOV_API_ENABLED_GUARD();

	ZVAL_LONG(return_value, 0);

	do {
		Z_LVAL_P(return_value) += (arena->end - arena->ptr);
	} while ((arena = arena->prev));
} /* }}} */

/* {{{ bool \pcov\enabled(void) */
PHP_NAMED_FUNCTION(php_pcov_enabled)
{
	if (zend_parse_parameters_none() != SUCCESS) {
		return;
	}

	RETURN_BOOL(php_pcov_api_enabled());
} /* }}} */

/* {{{ */
ZEND_BEGIN_ARG_INFO_EX(php_pcov_collect_arginfo, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, type, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, filter, IS_ARRAY, 0)
ZEND_END_ARG_INFO() /* }}} */

/* {{{ */
ZEND_BEGIN_ARG_INFO_EX(php_pcov_clear_arginfo, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, files, _IS_BOOL, 0)
ZEND_END_ARG_INFO() /* }}} */

/* {{{ */
ZEND_BEGIN_ARG_INFO_EX(php_pcov_no_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO() /* }}} */

/* {{{ php_pcov_functions[]
 */
const zend_function_entry php_pcov_functions[] = {
	ZEND_NS_FENTRY("pcov", start,      php_pcov_start,         php_pcov_no_arginfo, 0)
	ZEND_NS_FENTRY("pcov", stop,       php_pcov_stop,          php_pcov_no_arginfo, 0)
	ZEND_NS_FENTRY("pcov", collect,    php_pcov_collect,       php_pcov_collect_arginfo, 0)
	ZEND_NS_FENTRY("pcov", clear,      php_pcov_clear,         php_pcov_clear_arginfo, 0)
	ZEND_NS_FENTRY("pcov", waiting,    php_pcov_waiting,       php_pcov_no_arginfo, 0)
	ZEND_NS_FENTRY("pcov", memory,     php_pcov_memory,        php_pcov_no_arginfo, 0)
	ZEND_NS_FENTRY("pcov", enabled,    php_pcov_enabled,       php_pcov_no_arginfo, 0)
	PHP_FE_END
};
/* }}} */

/* {{{ pcov_module_deps[] */
static const zend_module_dep pcov_module_deps[] = {
	ZEND_MOD_REQUIRED("pcre")
	ZEND_MOD_END
}; /* }}} */

/* {{{ pcov_module_entry
 */
zend_module_entry pcov_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	pcov_module_deps,
	"pcov",
	php_pcov_functions,
	PHP_MINIT(pcov),
	PHP_MSHUTDOWN(pcov),
	PHP_RINIT(pcov),
	PHP_RSHUTDOWN(pcov),
	PHP_MINFO(pcov),
	PHP_PCOV_VERSION,
	PHP_MODULE_GLOBALS(pcov),
	PHP_GINIT(pcov),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_PCOV
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(pcov)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
