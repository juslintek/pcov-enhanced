Requirements
============

  * PHP 7.1+ (branch/path coverage requires PHP 8.0+)

About this build
================

This is **pcov-enhanced**, an alternative distribution of the `pcov` extension
that adds branch and path coverage. It builds and loads as `pcov` and is a
drop-in replacement: line coverage is unchanged, and `pcov.mode=branch` enables
branch/path coverage for `phpunit --path-coverage`. Only one of `pcov` and
`pcov-enhanced` can be installed at a time.

See [docs/04-distribution.md](docs/04-distribution.md) for every install route.

Installation
============

**From sources**

    git clone https://github.com/juslintek/pcov-enhanced.git
    cd pcov-enhanced
    phpize
    ./configure --enable-pcov
    make
    make test
    make install

**From a PECL tarball**

    pecl package
    pecl install pcov_enhanced-1.1.0.tgz

    # or straight from a release asset:
    pecl install https://github.com/juslintek/pcov-enhanced/releases/download/v1.1.0/pcov_enhanced-1.1.0.tgz

If upstream `pcov` is already installed, remove it first (`pecl uninstall pcov`)
because both provide the same `pcov` extension.

**Via Composer (php-ext)**

    composer require --dev juslintek/pcov-enhanced

Configuration
=============

    extension=pcov.so
    pcov.enabled=1
    ; default is "line" (unchanged from upstream pcov); set "branch" for
    ; branch + path coverage:
    pcov.mode=branch

Upstream pcov (line coverage only) remains available from PECL
(`pecl install pcov`), Fedora (`php-pecl-pcov`), and Debian/Ubuntu
(`php-pcov`) — see the upstream project at https://github.com/krakjoe/pcov.
