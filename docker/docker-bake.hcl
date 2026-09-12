# docker-bake.hcl — build pcov-enhanced images for every supported PHP version
# with a single command.
#
#   docker buildx bake -f docker/docker-bake.hcl                 # build all targets
#   docker buildx bake -f docker/docker-bake.hcl php83           # build one target
#   docker buildx bake -f docker/docker-bake.hcl --print         # print the plan (no build/pull)
#   docker buildx bake -f docker/docker-bake.hcl --push          # build + push (needs REGISTRY + creds)
#
# The build context is the repository root (..), and every target uses
# docker/Dockerfile with a different PHP_VERSION build arg. Because each image
# compiles the extension inside its own php:${PHP_VERSION} base, the resulting
# pcov.so always matches that image's PHP API version.
#
# NOTE: building pulls the official php:* base images from a registry, so it
# requires network access and cannot run in a fully offline environment.

# Registry/namespace the images are tagged under. Override with:
#   REGISTRY=ghcr.io/juslintek docker buildx bake -f docker/docker-bake.hcl
variable "REGISTRY" {
  default = "ghcr.io/juslintek"
}

# Image repository name. The extension is always `pcov`; the *image* is named
# after the product (pcov-enhanced).
variable "IMAGE" {
  default = "pcov-enhanced"
}

# Default group: build every supported PHP version.
group "default" {
  targets = ["php82", "php83", "php84"]
}

# Shared settings applied to each concrete target.
target "_common" {
  context    = ".."
  dockerfile = "docker/Dockerfile"
  target     = "final"
  platforms  = ["linux/amd64"]
}

target "php82" {
  inherits = ["_common"]
  args = {
    PHP_VERSION = "8.2"
  }
  tags = ["${REGISTRY}/${IMAGE}:8.2"]
}

target "php83" {
  inherits = ["_common"]
  args = {
    PHP_VERSION = "8.3"
  }
  tags = ["${REGISTRY}/${IMAGE}:8.3"]
}

target "php84" {
  inherits = ["_common"]
  args = {
    PHP_VERSION = "8.4"
  }
  # 8.4 is the default PHP_VERSION, so it also gets the moving `latest` tag.
  tags = ["${REGISTRY}/${IMAGE}:8.4", "${REGISTRY}/${IMAGE}:latest"]
}
