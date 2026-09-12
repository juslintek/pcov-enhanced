# Kubernetes examples for pcov-enhanced

Two ways to get the `pcov` extension (compiled from pcov-enhanced) into a pod.
Both build on the Docker images from [`../docker/`](../docker/): every image
compiles the extension as the standard, drop-in `pcov` extension, so PHPUnit
`--coverage-*` works unchanged and `pcov.mode=branch` unlocks
`phpunit --path-coverage` via the bundled Xdebug-compat shim.

| Manifest | Model | When to use |
| --- | --- | --- |
| [`prebuilt-image-example.yaml`](prebuilt-image-example.yaml) | Run a pcov-enhanced image directly. | Default. Simplest, no version-matching pitfalls. |
| [`initcontainer-example.yaml`](initcontainer-example.yaml) | Inject `pcov.so` into an existing PHP image. | You must keep an existing app image unchanged. |

## Prebuilt-image model (recommended)

Run a pcov-enhanced image (`ghcr.io/juslintek/pcov-enhanced:<php>`) as the app
or CI container. The extension is compiled **inside** that image, so its
`pcov.so` already matches the image's PHP API version — there is nothing to
inject and no compatibility problem to reason about. Layer your application on
top with a derived Dockerfile or a volume.

## Init-container injection model

The app container runs an **unmodified** upstream PHP image. An `initContainer`
built from the matching pcov-enhanced image copies its `pcov.so` and an ini
snippet into shared `emptyDir` volumes, which are mounted into the app
container's `extension_dir` and `conf.d`. PHP then loads `pcov` at startup with
no rebuild of the app image.

### ⚠️ PHP API version matching (the one thing that will bite you)

A compiled PHP extension `.so` only loads into a PHP binary whose **PHP API
version** (Zend module API number) matches the one it was built against. So the
pcov-enhanced initContainer image **must** be built for the same PHP minor
version — and the same ZTS/NTS + debug build — as the app container image. In
the example both are PHP 8.3.

A mismatch fails at load time with something like:

```
Unable to load dynamic library 'pcov.so' ... this extension was built with API=<X>, PHP compiled with API=<Y>
```

Check the API version in each image before pairing them:

```sh
docker run --rm ghcr.io/juslintek/pcov-enhanced:8.3 php -i | grep 'PHP API'
docker run --rm php:8.3-fpm                          php -i | grep 'PHP API'
```

The example resolves the destination `extension_dir` at runtime
(`php -r 'echo ini_get("extension_dir");'`) so it stays correct across PHP
**patch** releases; only the minor version has to match. The ini snippet is
mounted into `conf.d` via `subPath` so the base image's other conf.d files are
preserved. The prebuilt-image model avoids all of this because source and
destination are the same image.

## Applying and checking

```sh
kubectl apply -f k8s/prebuilt-image-example.yaml
# or
kubectl apply -f k8s/initcontainer-example.yaml

# Confirm pcov is loaded in the running pod:
kubectl exec deploy/app-with-pcov -c app -- php -m | grep pcov
```

Building and pushing the referenced images is a networked step — see
[`../docker/README.md`](../docker/README.md#publish-networked-step--ci-or-a-maintainer-runs-this).
