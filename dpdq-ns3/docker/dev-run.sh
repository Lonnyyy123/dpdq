#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="ubuntu:20.04"
SIM_BIN="${SIM_BIN:-${ROOT_DIR}/build/scratch/cb-simulator}"

if ! command -v docker >/dev/null 2>&1; then
  echo "Error: docker command not found." >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "Error: cannot access Docker daemon. Add current user to docker group or run this script with sudo." >&2
  exit 1
fi

if [[ ! -x "${SIM_BIN}" ]]; then
  echo "Error: ${SIM_BIN} not found or not executable." >&2
  echo "Run ./docker/dev-build.sh first." >&2
  exit 1
fi

RUN_ARGS=("$@")
HAS_INPUT_TEMPLATE=0
for arg in "${RUN_ARGS[@]}"; do
  if [[ "${arg}" == "--in" || "${arg}" == "-i" ]]; then
    HAS_INPUT_TEMPLATE=1
    break
  fi
done

if [[ ${HAS_INPUT_TEMPLATE} -eq 0 ]]; then
  RUN_ARGS=(--in /ns-3.19/template.config "${RUN_ARGS[@]}")
fi

TTY_FLAG=(-t)
if [[ -t 0 && -t 1 ]]; then
  TTY_FLAG=(-it)
fi

# Prefer a mirror with better connectivity in CN networks.
APT_MIRROR="${APT_MIRROR:-mirrors.tuna.tsinghua.edu.cn}"

# Avoid apt DNS resolution failures inside container.
DNS_ARGS=(--dns 8.8.8.8 --dns 1.1.1.1)
# Fallback host mappings for environments where container DNS is broken.
APT_HOST_ARGS=(
  --add-host archive.ubuntu.com:185.125.190.36
  --add-host security.ubuntu.com:91.189.91.82
)

echo "[dev-run] Running simulation in container ${IMAGE} ..."

docker run --rm "${TTY_FLAG[@]}" \
  --network host \
  -e APT_MIRROR="${APT_MIRROR}" \
  -e NS_LOG="${NS_LOG:-}" \
  "${DNS_ARGS[@]}" \
  "${APT_HOST_ARGS[@]}" \
  -v "${ROOT_DIR}:/ns-3.19" \
  -w /ns-3.19 \
  "${IMAGE}" \
  bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

if ! command -v python3 >/dev/null 2>&1 || ! ldconfig -p 2>/dev/null | grep -q "libgtk-3.so.0"; then
  if [[ -n "${APT_MIRROR:-}" ]]; then
    sed -i "s|http://archive.ubuntu.com/ubuntu|http://${APT_MIRROR}/ubuntu|g" /etc/apt/sources.list || true
    sed -i "s|http://security.ubuntu.com/ubuntu|http://${APT_MIRROR}/ubuntu|g" /etc/apt/sources.list || true
  fi

  apt-get update -qq || {
    echo "[dev-run] apt-get update failed (likely DNS/network issue)." >&2
    exit 100
  }
  apt-get install -y --no-install-recommends python3 libgtk-3-0 >/dev/null || {
    echo "[dev-run] Failed to install python3/libgtk-3-0." >&2
    exit 100
  }
fi

# Match docker/Dockerfile runtime layout when running directly from source mount.
[[ -f /ns-3.19/main.py ]] || cp /ns-3.19/docker/main.py /ns-3.19/main.py
[[ -f /ns-3.19/template.config ]] || cp /ns-3.19/docker/template.config /ns-3.19/template.config

if [[ ! -f /ns-3.19/workloads/DCTCP_CDF.txt ]]; then
  echo "[dev-run] Expected workload not found: /ns-3.19/workloads/DCTCP_CDF.txt" >&2
  echo "[dev-run] Check that the repository is mounted correctly." >&2
  exit 101
fi

python3 /ns-3.19/main.py "$@"
' _ "${RUN_ARGS[@]}"
