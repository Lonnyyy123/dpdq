#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCKER_DIR="${ROOT_DIR}/docker"
BASE_IMAGE="${BASE_IMAGE:-ubuntu:16.04}"
BUILD_ENV_IMAGE="${BUILD_ENV_IMAGE:-ns3-tlt-rdma:build-xenial}"
# Keep Ubuntu 16.04, but try a few mirrors because Xenial sources are flaky.
APT_MIRROR="${APT_MIRROR:-mirrors.aliyun.com}"

if ! command -v docker >/dev/null 2>&1; then
  echo "Error: docker command not found." >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "Error: cannot access Docker daemon. Add current user to docker group or run this script with sudo." >&2
  exit 1
fi

TTY_FLAG=(-t)
if [[ -t 0 && -t 1 ]]; then
  TTY_FLAG=(-it)
fi

echo "[build] Root directory: ${ROOT_DIR}"
echo "[build] Base image: ${BASE_IMAGE}"
echo "[build] Build environment image: ${BUILD_ENV_IMAGE}"
echo "[build] Preferred APT mirror: ${APT_MIRROR}"
echo "[build] Step 1/3: refreshing cached build environment image..."

docker build \
  -f "${DOCKER_DIR}/build-env.Dockerfile" \
  -t "${BUILD_ENV_IMAGE}" \
  --build-arg BASE_IMAGE="${BASE_IMAGE}" \
  --build-arg APT_MIRROR="${APT_MIRROR}" \
  "${DOCKER_DIR}"

echo "[build] Step 2/3: starting containerized build..."
echo "[build] Toolchain dependencies come from the cached Docker image."

docker run --rm "${TTY_FLAG[@]}" \
  -v "${ROOT_DIR}:/work" \
  -w /work \
  "${BUILD_ENV_IMAGE}" \
  bash -lc '
set -euo pipefail
echo "[build] Step 3/3: waf configure"
./waf configure --build-profile=debug

echo "[build] Step 3/3: waf build $*"
./waf build "$@"
  ' _ "$@"

echo "[build] Done. Build artifacts are in ${ROOT_DIR}/build"
