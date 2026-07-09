#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="ubuntu:16.04"

if ! command -v docker >/dev/null 2>&1; then
  echo "Error: docker command not found." >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "Error: cannot access Docker daemon. Add current user to docker group or run this script with sudo." >&2
  exit 1
fi

echo "[dev-build] Building ns-3 in container ${IMAGE} ..."

docker run --rm -t \
  -v "${ROOT_DIR}:/work" \
  -w /work \
  "${IMAGE}" \
  bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  python gcc-5 g++-5 make cmake pkg-config libc6-dev ca-certificates >/dev/null
update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-5 50
update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-5 50
./waf configure --build-profile=debug
./waf build "$@"
' _ "$@"

echo "[dev-build] Done. Build artifacts are in ${ROOT_DIR}/build"
