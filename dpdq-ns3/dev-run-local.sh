#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN_PY="${ROOT_DIR}/main.py"
SIM_BIN="${SIM_BIN:-${ROOT_DIR}/build/scratch/cb-simulator}"

if [[ ! -f "${MAIN_PY}" ]]; then
  echo "Error: ${MAIN_PY} not found." >&2
  exit 1
fi

if [[ ! -x "${SIM_BIN}" ]]; then
  echo "Error: ${SIM_BIN} not found or not executable." >&2
  echo "Run ./docker/dev-build.sh or your local build first." >&2
  exit 1
fi

if [[ -n "${PYTHON:-}" ]]; then
  PYTHON_BIN="${PYTHON}"
elif command -v python3 >/dev/null 2>&1; then
  PYTHON_BIN="python3"
elif command -v python >/dev/null 2>&1; then
  PYTHON_BIN="python"
else
  echo "Error: no Python interpreter found (python3/python)." >&2
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
  RUN_ARGS=(--in "${ROOT_DIR}/template.config" "${RUN_ARGS[@]}")
fi

echo "[dev-run-local] Running with ${PYTHON_BIN} using ${SIM_BIN} ..."

cd "${ROOT_DIR}"
exec "${PYTHON_BIN}" "${MAIN_PY}" "${RUN_ARGS[@]}"
