# Docker images for pcov-enhanced

These files build container images that are the official `php:${PHP_VERSION}-cli`
image **plus the `pcov` extension**, compiled from pcov-enhanced. Because the
extension compiles and loads as the standard `pcov` extension, every tool that
expects `pcov` (PHPUnit `--coverage-*`, Infection, etc.) works unchanged, and
`pcov.mode=branch` additionally unlocks `phpunit --path-coverage` through the
bundled Xdebug-compat shim.

| File | Purpose |
| --- | --- |
| [`Dockerfile`](Dockerfile) | Multi-stage build of `pcov` on `php:${PHP_VERSION}-cli`, parameterized by `PHP_VERSION`. |
| [`docker-bake.hcl`](docker-bake.hcl) | Build the 8.2 / 8.3 / 8.4 image matrix with one `docker buildx bake`. |
| [`compose.yaml`](compose.yaml) | Copy-pasteable dev example that mounts a project and runs coverage. |

> **PHP API version matching.** A PHP extension `.so` only loads into a PHP
> binary whose *PHP API version* matches the one it was built against. These
> images compile the extension **inside** the target `php:${PHP_VERSION}` image,
> so the match is guaranteed. If you instead inject a prebuilt `pcov.so` into a
> different image (see [`../k8s/`](../k8s/)), the two images must share the same
> PHP minor version and ZTS/NTS + debug flags. Check with `php -i | grep 'PHP API'`.

## Build a single variant

```sh
# From the repository root (build context = repo root, config.m4 is there):
docker build -f docker/Dockerfile --build-arg PHP_VERSION=8.4 -t pcov-enhanced:8.4 .
docker build -f docker/Dockerfile --build-arg PHP_VERSION=8.3 -t pcov-enhanced:8.3 .
docker build -f docker/Dockerfile --build-arg PHP_VERSION=8.2 -t pcov-enhanced:8.2 .
```

The default `PHP_VERSION` is `8.4`, so `docker build -f docker/Dockerfile -t pcov-enhanced:latest .`
also works.

## Build all variants with bake

```sh
# Print the resolved plan without building or pulling anything:
docker buildx bake -f docker/docker-bake.hcl --print

# Build every PHP version (8.2, 8.3, 8.4):
docker buildx bake -f docker/docker-bake.hcl

# Build just one:
docker buildx bake -f docker/docker-bake.hcl php83
```

## Prove pcov loads

```sh
# The extension is present and named `pcov`:
docker run --rm pcov-enhanced:8.4 php -m | grep pcov
# -> pcov

# Branch mode exposes the Xdebug-compat path-coverage surface:
docker run --rm pcov-enhanced:8.4 \
  php -d pcov.mode=branch -r 'var_dump(function_exists("xdebug_get_code_coverage"));'
# -> bool(true)
```

## Use as a CI base image

Point your CI job's container at a pcov-enhanced image and run PHPUnit normally:

```yaml
# GitHub Actions example
jobs:
  test:
    runs-on: ubuntu-latest
    container: ghcr.io/juslintek/pcov-enhanced:8.3
    steps:
      - uses: actions/checkout@v4
      - run: composer install --no-interaction
      # Line coverage (identical to upstream pcov):
      - run: php -d pcov.mode=line vendor/bin/phpunit --coverage-text
      # Path coverage (via the Xdebug-compat shim; far faster than Xdebug):
      - run: php -d pcov.mode=branch vendor/bin/phpunit --path-coverage
```

```yaml
# GitLab CI example
coverage:
  image: ghcr.io/juslintek/pcov-enhanced:8.3
  script:
    - composer install --no-interaction
    - php -d pcov.mode=branch vendor/bin/phpunit --path-coverage --coverage-text
```

## Dev usage with Compose

```sh
# Mount the project you want to measure at /app and check pcov is loaded:
PROJECT_DIR=/path/to/your/project docker compose -f docker/compose.yaml run --rm pcov

# Run your suite with path coverage:
PROJECT_DIR=/path/to/your/project docker compose -f docker/compose.yaml run --rm pcov \
  php -d pcov.mode=branch vendor/bin/phpunit --path-coverage
```

## Publish (networked step — CI or a maintainer runs this)

Building and pushing pulls the official `php:*` base images and pushes to a
registry, so it **requires network access and registry credentials** and cannot
run in an offline / registry-restricted environment.

```sh
# Log in to the registry (GitHub Container Registry shown):
echo "$CR_PAT" | docker login ghcr.io -u <user> --password-stdin

# Build + push every variant to the registry set in docker-bake.hcl (REGISTRY):
REGISTRY=ghcr.io/juslintek docker buildx bake -f docker/docker-bake.hcl --push

# Or per-version with plain docker:
docker build -f docker/Dockerfile --build-arg PHP_VERSION=8.3 -t ghcr.io/juslintek/pcov-enhanced:8.3 .
docker push ghcr.io/juslintek/pcov-enhanced:8.3
```
