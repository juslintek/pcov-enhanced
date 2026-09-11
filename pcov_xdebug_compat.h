/*
  +----------------------------------------------------------------------+
  | pcov xdebug-compat shim  (integration route (c), opt-in)             |
  +----------------------------------------------------------------------+
  | Registers XDEBUG_CC_* constants and xdebug_* coverage functions so   |
  | that unmodified php-code-coverage / PHPUnit can drive pcov through   |
  | its XdebugDriver for --path-coverage.                                |
  |                                                                      |
  | Guardrails (see docs/02-integration-decision.md):                    |
  |   * only active when pcov.mode=branch (env PCOV_MODE=branch)          |
  |   * never registers if the real Xdebug extension is loaded           |
  |   * phpversion('xdebug') reports a visibly-fake sentinel             |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_PCOV_XDEBUG_COMPAT_H
#define PHP_PCOV_XDEBUG_COMPAT_H

#include "php.h"

/* Sentinel version: >= 3.1 so XdebugDriver's version_compare passes, and
 * visibly not a real Xdebug release so nobody mistakes this for one. */
#define PCOV_XDEBUG_COMPAT_VERSION "3.99.0-pcov"

/* Called from MINIT. Registers the XDEBUG_CC_* / XDEBUG_FILTER_* /
 * XDEBUG_PATH_INCLUDE constants and appends xdebug_* function entries.
 * No-op unless branch mode is requested and real Xdebug is absent. */
void php_pcov_xdebug_compat_minit(INIT_FUNC_ARGS);

/* Whether the shim decided to activate (branch mode + no real xdebug). */
int php_pcov_xdebug_compat_active(void);

#endif /* PHP_PCOV_XDEBUG_COMPAT_H */
