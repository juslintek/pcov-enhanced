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

#ifndef PHP_PCOV_H
#define PHP_PCOV_H

extern zend_module_entry pcov_module_entry;
#define phpext_pcov_ptr &pcov_module_entry

/* Alternative pcov build: 1.0.x upstream base + branch/path coverage. The
 * extension still identifies as "pcov" (drop-in); the enhanced build is
 * distinguishable at runtime by its version, phpversion('pcov') == "1.1.0"
 * (upstream pcov is 1.0.x). */
#define PHP_PCOV_VERSION "1.1.0"

#ifdef PHP_WIN32
#	define PHP_PCOV_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_PCOV_API __attribute__ ((visibility("default")))
#else
#	define PHP_PCOV_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

typedef struct _php_coverage_t php_coverage_t;

struct _php_coverage_t {
	zend_string    *file;
	uint32_t        line;
	php_coverage_t *next;
};

/* Coverage granularity. PCOV_MODE_LINE is the historical default and is
 * byte-for-byte unchanged. PCOV_MODE_BRANCH additionally records reached
 * opcode indexes so \pcov\collect() (and the xdebug-compat shim) can emit
 * branch and path coverage. */
#define PCOV_MODE_LINE   0
#define PCOV_MODE_BRANCH 1

/* xdebug_set_filter list types, applied on top of pcov.directory scoping when
 * the extension is driven through the xdebug-compat surface. */
#define PCOV_FILTER_MODE_NONE    0   /* no path filter active */
#define PCOV_FILTER_MODE_INCLUDE 1   /* only files under a listed prefix */
#define PCOV_FILTER_MODE_EXCLUDE 2   /* all files except those under a prefix */

/* Xdebug $list_type values passed to xdebug_set_filter (must match Xdebug). */
#define PCOV_XDEBUG_LIST_PATH_INCLUDE 1
#define PCOV_XDEBUG_LIST_PATH_EXCLUDE 2

ZEND_BEGIN_MODULE_GLOBALS(pcov)
	zend_bool         enabled;
	int               mode;      /* PCOV_MODE_* resolved at RINIT */
	zend_arena       *mem;
	php_coverage_t   *start;
	php_coverage_t  **next;
	php_coverage_t  **last;
	HashTable         waiting;
	HashTable         files;
	HashTable         ignores;
	HashTable         wants;
	HashTable         discovered;
	HashTable         covered;
	HashTable         reached;   /* branch mode: op_array opcodes-ptr -> pcov_reached_t */
	void             *frames;    /* branch mode: runtime frame stack (pcov_frame_stack_t*) */
	int               filter_mode;   /* PCOV_FILTER_MODE_* from xdebug_set_filter */
	HashTable         filter_paths;  /* lowercased path prefixes for the filter  */
	zend_bool         filter_paths_init;
	zend_string      *directory;
	pcre_cache_entry *exclude;
	struct {
		zend_bool enabled;
		zend_long memory;
		zend_long files;
		char     *directory;
		char     *exclude;
		char     *mode;
	} ini;
ZEND_END_MODULE_GLOBALS(pcov)

#define PCG(v) ZEND_MODULE_GLOBALS_ACCESSOR(pcov, v)

#if defined(ZTS) && defined(COMPILE_DL_PCOV)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif	/* PHP_PCOV_H */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
