/*
  +----------------------------------------------------------------------+
  | pcov xdebug-compat shim  (see pcov_xdebug_compat.h)                  |
  +----------------------------------------------------------------------+
*/

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/pcre/php_pcre.h"
#include "zend_arena.h"
#include "Zend/zend_extensions.h"
#include "Zend/zend_constants.h"

#include "php_pcov.h"
#include "pcov_xdebug_compat.h"

/* pcov's own coverage entry points, implemented in pcov.c. We reuse them so
 * there is exactly one producer. */
extern void php_pcov_start_internal(void);
extern void php_pcov_stop_internal(int cleanup);
extern void php_pcov_collect_into(zval *return_value, zend_long type, zval *filter);
extern int  php_pcov_branch_mode_requested(void);
extern int  php_pcov_is_api_enabled(void);
extern void php_pcov_compat_set_filter(zend_long list_type, zval *configuration);

static int pcov_xdebug_compat_is_active = 0;

/* --- XDEBUG_CC_* flag values (must match Xdebug's public constants) ------- */
#define PCOV_XDEBUG_CC_UNUSED       (1 << 0)
#define PCOV_XDEBUG_CC_DEAD_CODE    (1 << 1)
#define PCOV_XDEBUG_CC_BRANCH_CHECK (1 << 2)

/* These MUST match Xdebug's real constant values, since php-code-coverage's
 * XdebugDriver passes them: XDEBUG_FILTER_CODE_COVERAGE=256 (0x100),
 * XDEBUG_PATH_INCLUDE=1, XDEBUG_PATH_EXCLUDE=2, XDEBUG_FILTER_NONE=0. */
#define PCOV_XDEBUG_FILTER_CODE_COVERAGE 256
#define PCOV_XDEBUG_FILTER_NONE          0
#define PCOV_XDEBUG_PATH_INCLUDE         1
#define PCOV_XDEBUG_PATH_EXCLUDE         2

int php_pcov_xdebug_compat_active(void)
{
	return pcov_xdebug_compat_is_active;
}

/* {{{ bool xdebug_start_code_coverage(int $flags = 0) */
static PHP_FUNCTION(pcov_compat_xdebug_start_code_coverage)
{
	zend_long flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &flags) != SUCCESS) {
		RETURN_THROWS();
	}
	/* We always collect branch+path in branch mode; the flags only influence
	 * which parts php-code-coverage later reads, so they need no handling
	 * here beyond acceptance. */
	(void) flags;
	php_pcov_start_internal();
	RETURN_TRUE;
}
/* }}} */

/* {{{ void xdebug_stop_code_coverage(bool $cleanup = true) */
static PHP_FUNCTION(pcov_compat_xdebug_stop_code_coverage)
{
	zend_bool cleanup = 1;
	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|b", &cleanup) != SUCCESS) {
		RETURN_THROWS();
	}
	/* Honor $cleanup: when true (Xdebug's default), discard the collected
	 * coverage so a subsequent XdebugDriver run starts clean; when false,
	 * preserve it. */
	php_pcov_stop_internal(cleanup ? 1 : 0);
}
/* }}} */

/* {{{ array xdebug_get_code_coverage() */
static PHP_FUNCTION(pcov_compat_xdebug_get_code_coverage)
{
	if (zend_parse_parameters_none() != SUCCESS) {
		RETURN_THROWS();
	}
	/* type 0 == \pcov\all; no filter. */
	php_pcov_collect_into(return_value, 0, NULL);
}
/* }}} */

/* {{{ void xdebug_set_filter(int $group, int $list_type, array $configuration) */
static PHP_FUNCTION(pcov_compat_xdebug_set_filter)
{
	zend_long group, list_type;
	zval *configuration;
	if (zend_parse_parameters(ZEND_NUM_ARGS(), "lla", &group, &list_type, &configuration) != SUCCESS) {
		RETURN_THROWS();
	}
	/* Only the code-coverage filter group is meaningful here. Apply Xdebug's
	 * path-prefix include/exclude semantics on top of pcov.directory scoping;
	 * XDEBUG_FILTER_NONE clears any active filter. */
	if (group != PCOV_XDEBUG_FILTER_CODE_COVERAGE) {
		return;
	}
	php_pcov_compat_set_filter(list_type, configuration);
}
/* }}} */

/* {{{ mixed xdebug_info([string $category]) */
static PHP_FUNCTION(pcov_compat_xdebug_info)
{
	char *category = NULL;
	size_t category_len = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|s", &category, &category_len) != SUCCESS) {
		RETURN_THROWS();
	}

	/* XdebugDriver::ensureXdebugIsAvailable() calls xdebug_info('mode') and
	 * requires 'coverage' to be present. */
	if (category && strcmp(category, "mode") == 0) {
		array_init(return_value);
		add_next_index_string(return_value, "coverage");
		return;
	}

	array_init(return_value);
}
/* }}} */

/* {{{ string xdebug_get_code_coverage_started_flags-ish stubs not needed */

ZEND_BEGIN_ARG_INFO_EX(pcov_compat_ai_start, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, flags, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(pcov_compat_ai_stop, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, cleanup, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(pcov_compat_ai_none, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(pcov_compat_ai_filter, 0, 0, 3)
	ZEND_ARG_TYPE_INFO(0, group, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, list_type, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, configuration, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(pcov_compat_ai_info, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, category, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry pcov_xdebug_compat_functions[] = {
	ZEND_FENTRY(xdebug_start_code_coverage, ZEND_FN(pcov_compat_xdebug_start_code_coverage), pcov_compat_ai_start, 0)
	ZEND_FENTRY(xdebug_stop_code_coverage,  ZEND_FN(pcov_compat_xdebug_stop_code_coverage),  pcov_compat_ai_stop, 0)
	ZEND_FENTRY(xdebug_get_code_coverage,   ZEND_FN(pcov_compat_xdebug_get_code_coverage),   pcov_compat_ai_none, 0)
	ZEND_FENTRY(xdebug_set_filter,          ZEND_FN(pcov_compat_xdebug_set_filter),          pcov_compat_ai_filter, 0)
	ZEND_FENTRY(xdebug_info,                ZEND_FN(pcov_compat_xdebug_info),                pcov_compat_ai_info, 0)
	PHP_FE_END
};

/* A minimal internal module named "xdebug" so that extension_loaded('xdebug')
 * is true and phpversion('xdebug') returns our sentinel. It carries the
 * compat functions. */
static zend_module_entry pcov_fake_xdebug_module_entry = {
	STANDARD_MODULE_HEADER,
	"xdebug",
	pcov_xdebug_compat_functions,
	NULL, NULL, NULL, NULL, NULL,
	PCOV_XDEBUG_COMPAT_VERSION,
	STANDARD_MODULE_PROPERTIES
};

void php_pcov_xdebug_compat_minit(INIT_FUNC_ARGS)
{
	pcov_xdebug_compat_is_active = 0;

	/* Opt-in only, and only when PCOV itself is enabled. If PCOV is disabled
	 * (pcov.enabled=0 / PCOV_ENABLED=0), RINIT skips table setup, so registering
	 * the fake xdebug surface would let xdebug_get_code_coverage() return an
	 * empty array and PHPUnit silently report zero coverage. */
	if (!php_pcov_branch_mode_requested() || !php_pcov_is_api_enabled()) {
		return;
	}

	/* Never shadow the real Xdebug. If it is loaded, defer entirely. */
	if (zend_hash_str_exists(&module_registry, "xdebug", sizeof("xdebug") - 1)) {
		return;
	}
	/* Xdebug is a Zend extension; its module may register after us. Also
	 * refuse if the Zend extension is present, so we never double-register a
	 * module named "xdebug". */
	if (zend_get_extension("Xdebug") != NULL) {
		return;
	}

	/* Register the XDEBUG_CC_* / filter constants the driver references. */
	REGISTER_LONG_CONSTANT("XDEBUG_CC_UNUSED",       PCOV_XDEBUG_CC_UNUSED,       CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_CC_DEAD_CODE",    PCOV_XDEBUG_CC_DEAD_CODE,    CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_CC_BRANCH_CHECK", PCOV_XDEBUG_CC_BRANCH_CHECK, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_FILTER_CODE_COVERAGE", PCOV_XDEBUG_FILTER_CODE_COVERAGE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_PATH_INCLUDE",    PCOV_XDEBUG_PATH_INCLUDE,    CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_PATH_EXCLUDE",    PCOV_XDEBUG_PATH_EXCLUDE,    CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("XDEBUG_FILTER_NONE",     PCOV_XDEBUG_FILTER_NONE,     CONST_CS | CONST_PERSISTENT);

	/* Register the stand-in "xdebug" module (also registers its functions). */
	if (zend_register_internal_module(&pcov_fake_xdebug_module_entry) != NULL) {
		pcov_xdebug_compat_is_active = 1;
	}
}
