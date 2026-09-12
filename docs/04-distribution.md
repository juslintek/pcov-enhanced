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

The GitHub Actions workflow already builds the extension across PHP 8.2–8.4. To
ship binaries, extend it to `actions/upload-artifact` / attach to a GitHub
Release the built `modules/pcov.so` per PHP version/OS, and (optionally) Windows
DLLs via the existing AppVeyor pipeline. Consumers can then drop the `.so`/`.dll`
into their extension dir without a toolchain. `setup-php` users can point at the
release asset.

## 6. Distro packages (downstream)

Upstream pcov ships in Fedora (`php-pecl-pcov`), Debian/Ubuntu (Sury/ondrej
`php-pcov`), etc. Those are maintained by distro packagers from the official
PECL release. For this alternative build the realistic paths are:
- a personal repo / PPA that ships `php-pcov-enhanced`, or
- upstreaming the feature into `krakjoe/pcov` (see PR) so it reaches those
  channels through the normal pcov release — the preferred long-term route.

## Coexistence & safety

- **Real Xdebug present:** the xdebug-compat surface does not register and
  pcov-enhanced falls back to line mode; Xdebug provides branch/path coverage.
- **Default mode:** `pcov.mode=line` — no xdebug symbols, identical to upstream
  pcov. Nothing changes for existing line-coverage users.
- **Version check at runtime:** `phpversion('pcov')` reports `1.1.0` for this
  build; in branch mode `phpversion('xdebug')` reports `3.99.0-pcov`.
