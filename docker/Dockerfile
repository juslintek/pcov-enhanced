# syntax=docker/dockerfile:1
#
# pcov-enhanced — build the extension as the drop-in `pcov` extension on top of
# an official php:${PHP_VERSION}-cli image.
#
# This follows the standard docker-library/php extension build pattern:
#   - docker-php-source extract   (unpack the PHP source tree the base image ships)
#   - install $PHPIZE_DEPS        (autoconf / gcc / make / etc. provided by the base image)
#   - phpize && ./configure --enable-pcov && make && make install
#   - enable via a conf.d snippet (extension=pcov.so, pcov.enabled=1)
#
# The result is an image that is identical to the upstream php:${PHP_VERSION}-cli
# image plus the `pcov` extension (compiled from pcov-enhanced), so every tool
# that expects the `pcov` extension keeps working unchanged.
#
# IMPORTANT — PHP API version matching:
#   A compiled PHP extension (.so) is only loadable by a PHP binary whose
#   *PHP API version* (Zend module API no.) matches the one it was built against.
#   That is why the extension is compiled INSIDE the target php:${PHP_VERSION}
#   image here — the .so is guaranteed to match. If you instead copy a prebuilt
#   pcov.so into a different image (see ../k8s/initcontainer-example.yaml), the
#   source and destination images MUST share the same PHP API version, which in
#   practice means the same PHP minor version (e.g. 8.3.x <-> 8.3.x) and the same
#   ZTS/NTS + debug flags. Check with:  php -i | grep 'PHP API'
#
# Build for a specific PHP version:
#   docker build -f docker/Dockerfile --build-arg PHP_VERSION=8.3 -t pcov-enhanced:8.3 .
#
# NOTE: building requires pulling the official php:* base image from a registry
# (network access). It cannot run in a fully offline / registry-restricted
# environment.

# ---------------------------------------------------------------------------
# Stage 1: build — compile pcov-enhanced as pcov inside the target PHP image.
# ---------------------------------------------------------------------------
ARG PHP_VERSION=8.4
FROM php:${PHP_VERSION}-cli AS build

# The source directory the extension is copied into inside the image.
WORKDIR /usr/src/pcov-enhanced

# Copy the extension source (config.m4 lives at the repo root, which is where
# phpize expects it). .dockerignore keeps build artifacts out of the context.
COPY . .

# Install build deps, compile, install, then strip the build toolchain again so
# nothing but the compiled artifacts survives into the final image layer.
#   $PHPIZE_DEPS is provided by the official php image and expands to the
#   autoconf/gcc/make/etc. packages needed to build extensions.
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends $PHPIZE_DEPS; \
    rm -rf /var/lib/apt/lists/*; \
    # Ensure a clean tree even if the build context carried host artifacts.
    phpize --clean || true; \
    phpize; \
    ./configure --enable-pcov --with-php-config="$(command -v php-config)"; \
    make -j"$(nproc)"; \
    make install; \
    # Record where PHP installed the extension so the final stage can grab it.
    php -r 'echo ini_get("extension_dir");' > /tmp/extension_dir

# Provide the conf.d snippet that turns pcov on. docker-php-ext-enable would
# normally write this; we write it explicitly so the exact contents are visible.
RUN set -eux; \
    { \
        echo '; pcov-enhanced (compiled and loaded as the standard `pcov` extension)'; \
        echo 'extension=pcov.so'; \
        echo 'pcov.enabled=1'; \
        echo '; Opt in to branch + path coverage (default "line" is byte-for-byte upstream pcov):'; \
        echo ';pcov.mode=branch'; \
    } > "$PHP_INI_DIR/conf.d/docker-php-ext-pcov.ini"

# Prove the extension loads as `pcov` at build time (fails the build otherwise).
RUN php -m | grep -qx pcov \
    && php -d pcov.mode=branch -r 'exit(function_exists("xdebug_get_code_coverage") ? 0 : 1);'

# ---------------------------------------------------------------------------
# Stage 2: final — start clean from the same base and copy just the artifacts.
# This keeps $PHPIZE_DEPS out of the shipped image.
# ---------------------------------------------------------------------------
ARG PHP_VERSION=8.4
FROM php:${PHP_VERSION}-cli AS final

# Copy the compiled extension into the correct extension_dir of THIS image.
# Both stages use the same php:${PHP_VERSION} base, so the PHP API versions
# match and the .so is loadable. On the official php images $PHP_INI_DIR is
# /usr/local/etc/php and the extension_dir lives under /usr/local/lib/php/extensions.
COPY --from=build /usr/local/lib/php/extensions/ /usr/local/lib/php/extensions/
COPY --from=build /usr/local/etc/php/conf.d/docker-php-ext-pcov.ini /usr/local/etc/php/conf.d/docker-php-ext-pcov.ini

# Sanity check in the final image as well.
RUN php -m | grep -qx pcov

# Default command prints the loaded extension so `docker run <img>` is a smoke test.
CMD ["php", "-m"]
