# Repository guidance

This repository builds the PCOV PHP extension in C. Read `INSTALL.md` and `.github/workflows/ci.yml` before changing the build or test setup; `docs/01-contract.md` describes the branch/path coverage API.

- Build against the selected PHP ABI with `phpize`, `./configure --enable-pcov --with-php-config="$(command -v php-config)"`, and `make`. Load `modules/pcov.so` explicitly for local checks; do not install it into the host PHP configuration while developing.
- Run both PHPT modes used by CI:

  ```sh
  NO_INTERACTION=1 REPORT_EXIT_STATUS=1 TEST_PHP_ARGS="-d extension=$(pwd)/modules/pcov.so -d pcov.enabled=1 -q --show-diff" php run-tests.php tests/
  NO_INTERACTION=1 REPORT_EXIT_STATUS=1 TEST_PHP_ARGS="-d extension=$(pwd)/modules/pcov.so -d pcov.enabled=1 -d pcov.mode=branch -q --show-diff" php run-tests.php tests/
  ```

  The CI matrix covers PHP 8.2–8.4 with opcache off and on; preserve its Valgrind run for C memory changes.
- Keep the line mode behavior compatible with upstream PCOV. Changes to branch/path output need PHPT coverage for the affected control-flow case and should remain compatible with php-code-coverage's expected shape.
- Do not skip tests or weaken expected results to make a run pass.
