# Distribution — pcov-enhanced (alternative pcov build)

**pcov-enhanced is an alternative distribution of the `pcov` extension.** It
compiles and loads as `pcov` (so it is a drop-in replacement), keeps line
coverage byte-for-byte identical to upstream, and adds opt-in branch and path
coverage (`pcov.mode=branch`) that is consumable through
`phpunit --path-coverage` — like Xdebug's path coverage, but far faster.

Because it *is* the `pcov` extension internally, **only one of `pcov` and
`pcov-enhanced` may be installed at a time.** Installing this replaces pcov.

> **On the GitHub repository name.** The repository stays named
> `pcov-enhanced`. Package identity does **not** depend on the repo name: it
> comes from `package.xml` (`pcov_enhanced` on PECL) and `composer.json`
> (`juslintek/pcov-enhanced` on Packagist / PIE), while the compiled extension
> is always `pcov`. Renaming the GitHub repo is optional and can only be done
> manually by a maintainer in GitHub settings (the release automation token
> cannot rename repositories); none of the packaging or install routes above
> require it.

---

## 1. Build from source

```sh
git clone https://github.com/juslintek/pcov-enhanced.git
cd pcov-enhanced
phpize
./configure --enable-pcov
make
make test           # optional
sudo make install
```

Then enable it in php.ini (or a conf.d snippet):

```ini
extension=pcov.so
pcov.enabled=1
; opt in to branch + path coverage (default is "line", unchanged from pcov):
pcov.mode=branch
```

## 2. PECL / a `.tgz`

A PECL-format `package.xml` is included; the package name is `pcov_enhanced`
(it *provides* the `pcov` extension). Because the official `pcov` name on
`pecl.php.net` belongs to upstream, this build is distributed as its own
package/tarball rather than through the official pcov channel.

Build the tarball and install it directly:

```sh
pecl package                       # produces pcov_enhanced-<version>.tgz
sudo pecl install pcov_enhanced-<version>.tgz
```

Install straight from a release asset:

```sh
sudo pecl install https://github.com/juslintek/pcov-enhanced/releases/download/v1.1.0/pcov_enhanced-1.1.0.tgz
```

If pcov is already installed, uninstall it first (`sudo pecl uninstall pcov`)
since both provide the same `pcov` extension.

## 3. Composer (`php-ext`)

`composer.json` declares the modern `type: php-ext` metadata. It sets
`replace`/`conflict` on `pecl/pcov` so Composer treats it as the pcov provider:

```sh
composer require --dev juslintek/pcov-enhanced
```

To publish on Packagist: submit the repository URL at
<https://packagist.org/packages/submit>; tag releases as `vX.Y.Z` so Packagist
picks them up. The `extension-name` stays `pcov` so PHP tooling that looks for
the `pcov` extension keeps working.

## 4. PIE (PHP Installer for Extensions)

[PIE](https://github.com/php/pie) is the modern successor to `pecl install`
for PHP 8.3+. It consumes the **same** `composer.json` `php-ext` metadata
(`type: php-ext`, `php-ext.extension-name`, `php-ext.configure-options`,
`php-ext.priority`), resolves the package from Packagist/GitHub, and **builds
from source**, installing it as the `pcov` extension. Because it reuses the
Composer metadata, no PIE-specific manifest is required.

```sh
pie install juslintek/pcov-enhanced
```

Notes:
- PIE downloads the package from Packagist/GitHub and compiles it, so it
  **requires network access** and a working build toolchain (`phpize`,
  compiler). It cannot run in a fully offline environment.
- `config.m4` lives at the **repository root**, which is PIE's default build
  path, so **no `php-ext.build-path` override is needed** in `composer.json`.
- The `extension-name` stays `pcov`, and `replace`/`conflict` on `pecl/pcov`
  ensure PIE/Composer treat this build as the pcov provider (install one at a
  time). Enable it in php.ini exactly as in the source-build route above
  (`extension=pcov.so`).

## 5. Prebuilt binaries via CI (recommended for consumers)

On every tag push, CI builds the extension across PHP 8.2–8.4 and attaches the
per-PHP-version `modules/pcov.so` to the release as a named asset that encodes
the extension version, PHP version, OS, architecture and thread-safety, e.g.:

```
pcov-pcov-enhanced-1.1.0-php8.3-linux-x86_64-nts.so
pcov-pcov-enhanced-1.1.0-php8.3-linux-x86_64-nts.so.sha256
```

A combined `SHA256SUMS.txt` covering the PECL tarball and every `.so` is also
attached so consumers can verify downloads. On GitHub these are produced by the
`prebuilt-binaries` + `checksums` jobs in `.github/workflows/ci.yml`; on GitLab
by the `package` + `release` stages in `.gitlab-ci.yml`.

> **Which checksum is authoritative?** Both the combined `SHA256SUMS.txt` and
> the per-`.so` `.sha256` sidecars are generated from the same `sha256sum` run,
> so they always agree. Treat `SHA256SUMS.txt` as the authoritative record: it
> is a single file covering the tarball **and** every `.so`, verifiable in one
> pass with `sha256sum -c SHA256SUMS.txt`. The per-`.so` `.sha256` sidecars are
> a convenience for consumers who download only one binary (as the setup-php
> recipe below does) and want to verify it without fetching the full list. Consumers can drop the
`.so` straight into their extension dir without a toolchain (Windows DLLs can
additionally be shipped via the AppVeyor pipeline).

> **PHP API version matching.** A prebuilt `.so` only loads into a PHP binary
> with the same PHP API version (and matching ZTS/NTS + debug flag). Always
> download the asset whose `phpX.Y` and `nts`/`zts` fields match your runtime.

### `setup-php` consumption recipe (no toolchain)

A downstream job can install this build without compiling by downloading the
prebuilt `.so` for its PHP version and enabling it. This is faster than the
PECL/PIE route (which builds from source and needs a compiler).

**GitHub Actions** (uses `shivammathur/setup-php`):

```yaml
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - id: php
        uses: shivammathur/setup-php@v2
        with:
          php-version: "8.3"
          coverage: none            # do NOT let setup-php install upstream pcov/xdebug
      - name: Install prebuilt pcov-enhanced
        env:
          VERSION: "1.1.0"          # the pcov-enhanced release to consume
        run: |
          php_minor="$(php -r 'echo PHP_MAJOR_VERSION.".".PHP_MINOR_VERSION;')"
          arch="$(uname -m)"
          ts="$(php -r 'echo PHP_ZTS ? "zts" : "nts";')"
          asset="pcov-pcov-enhanced-${VERSION}-php${php_minor}-linux-${arch}-${ts}.so"
          base="https://github.com/juslintek/pcov-enhanced/releases/download/v${VERSION}"
          curl -fSL -o pcov.so "${base}/${asset}"
          curl -fSL -o pcov.so.sha256 "${base}/${asset}.sha256"
          # Verify the download against the published checksum.
          echo "$(cut -d' ' -f1 pcov.so.sha256)  pcov.so" | sha256sum -c -
          ext_dir="$(php -i | sed -n 's/^extension_dir => \([^ ]*\).*/\1/p')"
          sudo cp pcov.so "${ext_dir}/pcov.so"
          printf 'extension=pcov.so\npcov.enabled=1\npcov.mode=branch\n' \
            | sudo tee "$(php --ini | sed -n 's/.*: //p' | head -n1)/../conf.d/99-pcov.ini" >/dev/null
      - run: php -m | grep -i pcov
```

**GitLab CI** (official `php:8.3` image):

```yaml
test:
  image: php:8.3
  variables:
    VERSION: "1.1.0"
  script:
    - php_minor="$(php -r 'echo PHP_MAJOR_VERSION.".".PHP_MINOR_VERSION;')"
    - arch="$(uname -m)"; ts="$(php -r 'echo PHP_ZTS ? "zts" : "nts";')"
    - asset="pcov-pcov-enhanced-${VERSION}-php${php_minor}-linux-${arch}-${ts}.so"
    - base="https://gitlab.com/juslintek/pcov-enhanced/-/releases/v${VERSION}/downloads"
    - curl -fSL -o pcov.so "${base}/${asset}"
    - ext_dir="$(php -r 'echo ini_get("extension_dir");')"
    - cp pcov.so "${ext_dir}/pcov.so"
    - docker-php-ext-enable pcov || echo "extension=pcov.so" > "$PHP_INI_DIR/conf.d/99-pcov.ini"
    - php -m | grep -i pcov
```

Prefer this route when you only need to *use* the extension. Use the PECL
tarball (section 2) or PIE (section 4) when you need a source build (unmatched
PHP API version, custom `configure` flags, or an unsupported platform).

## 5a. Release automation & publishing

Cutting a release is a version bump + a tag; CI does the rest. The manual
networked publish steps (PECL, Packagist) are listed explicitly at the end.

### Step 1 — Bump the version (one commit)

Update the version in all three places and set the PECL release date:

- `package.xml`: the `<version><release>X.Y.Z</release></version>` value **and**
  `<date>YYYY-MM-DD</date>` (must be *today* in UTC, or `pecl
  package-validate` warns "Release Date is not today").
- `composer.json`: no hardcoded version is required (Packagist derives it from
  the git tag), but update any `support`/changelog references if present.
- `CHANGELOG.md`: move the `[Unreleased]` entries under a new
  `## [X.Y.Z] - YYYY-MM-DD` heading.
- Runtime version sentinel: `phpversion('pcov')` is defined in `php_pcov.h`; if
  the release changes it, bump it there too.

```sh
git add package.xml composer.json CHANGELOG.md
git commit -m "chore(release): pcov-enhanced X.Y.Z"
```

### Step 2 — Tag and push

```sh
git tag -a vX.Y.Z -m "pcov-enhanced X.Y.Z"
git push origin main
git push origin vX.Y.Z          # the tag is what triggers the release jobs
```

### Step 3 — CI publishes automatically (no human action)

- **GitHub Actions** (`.github/workflows/ci.yml`): on the `refs/tags/*` push,
  the `package` job attaches the PECL tarball `pcov_enhanced-X.Y.Z.tgz`, the
  `prebuilt-binaries` job attaches one `pcov-pcov-enhanced-X.Y.Z-phpM.m-linux-<arch>-<ts>.so`
  (+ `.sha256`) per PHP version, and the `checksums` job attaches a combined
  `SHA256SUMS.txt`. `contents: write` is scoped to those release/packaging jobs
  only; every third-party action is SHA-pinned.
- **GitLab CI** (`.gitlab-ci.yml`): the `package` stage builds the tarball and a
  prebuilt `.so`, writes the combined `SHA256SUMS.txt`, and exports its own job
  id (`PACKAGE_JOB_ID`) plus the exact artifact filenames via a `dotenv`
  report. The tag-gated `release` stage (`rules: if $CI_COMMIT_TAG`) then
  creates a GitLab Release via `release-cli` with asset links to all three
  deliverables — the PECL tarball, the prebuilt `.so`, and `SHA256SUMS.txt` —
  each pointing at the **package** job's raw artifacts (via `${PACKAGE_JOB_ID}`,
  not the release job's own id), giving parity with the GitHub release.

### Step 4 — MANUAL networked publish steps

These require credentials/network and are **not** automated:

**Packagist** (Composer / PIE distribution) — one-time submission, then
tag-driven auto-update:

```sh
# One time only: submit the repository URL (needs a Packagist account).
#   open https://packagist.org/packages/submit
#   paste: https://github.com/juslintek/pcov-enhanced
#
# After submission, install the GitHub service hook so future tags auto-update:
#   GitHub repo → Settings → Webhooks → Add webhook
#     Payload URL:  https://packagist.org/api/github?username=<PACKAGIST_USER>
#     Content type: application/json
#     Secret:       <your Packagist API token>
#
# Or trigger a one-off update from the CLI:
curl -XPOST -H 'content-type:application/json' \
  "https://packagist.org/api/update-package?username=<PACKAGIST_USER>&apiToken=<API_TOKEN>" \
  -d '{"repository":{"url":"https://github.com/juslintek/pcov-enhanced"}}'
```

**PECL** (only if publishing through a PECL channel; the official `pcov` name on
`pecl.php.net` belongs to upstream, so this build ships as `pcov_enhanced`):

```sh
# Validate and build the exact tarball CI produced (sanity check locally):
pecl package-validate package.xml
pecl package package.xml                 # -> pcov_enhanced-X.Y.Z.tgz

# Publish to a PECL channel you control (requires a pecl.php.net / channel
# account with karma on the package). Log in, then upload the release:
pear login                               # prompts for pecl.php.net credentials
pecl release pcov_enhanced-X.Y.Z.tgz     # uploads the tarball to the channel
# (For a private/self-hosted channel, replace with `pirum add`/your channel's
# upload command. Distro packagers pick the tarball up from the release assets.)
```

Consumers who do not use a PECL channel can always install straight from the
GitHub/GitLab release asset (see section 2).

## 6. Distro packages (downstream)

Upstream pcov ships in Fedora (`php-pecl-pcov`), Debian/Ubuntu (Sury/ondrej
`php-pcov`), etc. Those are maintained by distro packagers from the official
PECL release. For this alternative build the realistic paths are:
- a personal repo / PPA that ships `php-pcov-enhanced`, or
- upstreaming the feature into `krakjoe/pcov` (see PR) so it reaches those
  channels through the normal pcov release — the preferred long-term route.

## 7. Docker & Kubernetes

Ready-to-use container artifacts live in [`docker/`](../docker/) and
[`k8s/`](../k8s/). They build and ship the extension as the standard, drop-in
`pcov` extension on top of the official `php:*` images, so every tool that
expects `pcov` (PHPUnit `--coverage-*`, Infection, …) works unchanged, and
`pcov.mode=branch` additionally unlocks `phpunit --path-coverage` through the
Xdebug-compat shim.

- **[`docker/Dockerfile`](../docker/Dockerfile)** — multi-stage build,
  parameterized by `ARG PHP_VERSION` (default 8.4), that compiles pcov-enhanced
  inside a `php:${PHP_VERSION}-cli` image via `phpize && ./configure
  --enable-pcov && make && make install` and enables it with a conf.d snippet
  (`extension=pcov.so`, `pcov.enabled=1`).
- **[`docker/docker-bake.hcl`](../docker/docker-bake.hcl)** — builds the PHP
  8.2 / 8.3 / 8.4 image matrix with a single `docker buildx bake`.
- **[`docker/compose.yaml`](../docker/compose.yaml)** — dev example that mounts
  a project and runs coverage.
- **[`docker/README.md`](../docker/README.md)** — build/run/publish commands and
  CI-base-image recipes (GitHub Actions + GitLab CI).

Two container delivery models are documented under [`k8s/`](../k8s/):

1. **Image-based (recommended)** —
   [`k8s/prebuilt-image-example.yaml`](../k8s/prebuilt-image-example.yaml) runs a
   pcov-enhanced image directly. The `.so` is compiled inside that image, so it
   always matches the image's PHP API version; there is nothing to inject.
2. **Init-container injection** —
   [`k8s/initcontainer-example.yaml`](../k8s/initcontainer-example.yaml) keeps an
   existing PHP image unchanged and uses an `initContainer` (built from the
   matching pcov-enhanced image) to copy `pcov.so` + a pcov `.ini` into a shared
   `emptyDir` mounted at the app container's extension/conf.d dir.

> **PHP API version matching.** A compiled `.so` only loads into a PHP binary
> whose PHP API version matches the build. The image-based model is safe by
> construction; the init-container model requires the source and destination
> images to share the same PHP minor version (and ZTS/NTS + debug build). See
> [`k8s/README.md`](../k8s/README.md) for details.

> **Offline note.** `docker build` / `docker buildx bake` pull the official
> `php:*` base images and therefore require network access; they are the
> networked steps a maintainer or CI runs (exact commands are in
> [`docker/README.md`](../docker/README.md)). The manifests and Dockerfile are
> validated structurally (YAML parse, `bake --print`) in this repo's tooling.

## 8. Coverage-tooling compatibility & the native-API roadmap

Distribution is only half the adoption story; the other half is which
coverage-consuming tools work against this build and where the ecosystem goes
next. Two companion documents cover that:

- **[`05-tooling-compatibility.md`](05-tooling-compatibility.md)** — a
  compatibility matrix for PHPUnit/php-code-coverage, Infection, Paratest,
  Codeception, Behat, and the Coveralls/Codecov/Scrutinizer uploaders: how each
  selects a driver, whether line and branch/path work, caveats, and the
  real-Xdebug coexistence rule. Uploaders are driver-agnostic (they consume the
  Clover/Cobertura XML php-code-coverage emits), so the prebuilt-binary and
  container routes above deliver working coverage uploads unchanged.
- **[`06-future-native-api.md`](06-future-native-api.md)** — the forward-looking
  clean route (a): a php-code-coverage `Selector` capability-probe plus a native
  `PcovBranchDriver` (no Xdebug impersonation), a leaner native collection API,
  and the infrastructure adoption requirements (what php-code-coverage, PHPUnit,
  `setup-php`, and the distro/PIE routes documented above would each need) with a
  phased migration plan.

## Coexistence & safety

- **Real Xdebug present:** the xdebug-compat surface does not register and
  pcov-enhanced falls back to line mode; Xdebug provides branch/path coverage.
- **Default mode:** `pcov.mode=line` — no xdebug symbols, identical to upstream
  pcov. Nothing changes for existing line-coverage users.
- **Version check at runtime:** `phpversion('pcov')` reports `1.1.0` for this
  build; in branch mode `phpversion('xdebug')` reports `3.99.0-pcov`.
