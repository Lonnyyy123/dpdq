#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="${ROOT_DIR}/dev-run-local.sh"
SIM_BIN="${SIM_BIN:-${ROOT_DIR}/build/scratch/exp5}"
CFG_DIR="${ROOT_DIR}/config/generated-exp5"
LOG_DIR="${ROOT_DIR}/logs"
MIX_ROOT="${ROOT_DIR}/mix"
STATE_DIR="${ROOT_DIR}/.runstate"
TRACE_FILE_DEFAULT="${ROOT_DIR}/mix/trace.txt"

usage() {
  cat <<USAGE
Usage:
  $(basename "$0") --algo <dpdq-ccfc|dpdq-fc|pfc|pfc-dcqcn|pfc-hpcc> --workload <websearch|hadoop> [--load <float>] [--receiver-strategy <rr|srpt>] [--mix-subdir <name>] [--dry-run]

Options:
  --algo       Exp5 algorithm to run
  --workload   Background workload to run
  --load       Offered background load, default 0.60
  --receiver-strategy
               CreditBouncer receiver-side grant policy, default rr
  --mix-subdir
               Place outputs under mix/<name>/ instead of mix/
  --dry-run    Generate config only, do not run simulator

Examples:
  $(basename "$0") --algo pfc-dcqcn --workload websearch
  $(basename "$0") --algo dpdq-ccfc --workload hadoop --load 0.60
  $(basename "$0") --algo dpdq-ccfc --workload hadoop --receiver-strategy srpt
  $(basename "$0") --algo dpdq-ccfc --workload hadoop --mix-subdir exp5
  $(basename "$0") --algo dpdq-fc --workload hadoop --load 0.60
  $(basename "$0") --algo pfc --workload hadoop --load 0.60
USAGE
}

ALGO=""
WORKLOAD=""
LOAD="0.60"
RECEIVER_STRATEGY="rr"
MIX_SUBDIR=""
DRY_RUN=0
APP_START_TIME="0.00"
APP_STOP_TIME="0.09"
ENABLE_FOREGROUND_INCAST="1"
FOREGROUND_INCAST_START_TIME="0.03"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --algo)
      ALGO="${2:-}"
      shift 2
      ;;
    --workload)
      WORKLOAD="${2:-}"
      shift 2
      ;;
    --load)
      LOAD="${2:-}"
      shift 2
      ;;
    --receiver-strategy)
      RECEIVER_STRATEGY="${2:-}"
      shift 2
      ;;
    --mix-subdir)
      MIX_SUBDIR="${2:-}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Error: unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "${ALGO}" || -z "${WORKLOAD}" ]]; then
  echo "Error: --algo and --workload are required" >&2
  usage >&2
  exit 1
fi

if [[ ! -x "${RUNNER}" ]]; then
  echo "Error: runner not executable: ${RUNNER}" >&2
  exit 1
fi

if [[ ! -x "${SIM_BIN}" ]]; then
  echo "Error: ${SIM_BIN} not found or not executable." >&2
  echo "Build the project first so exp5 is available." >&2
  exit 1
fi

if [[ ! -f "${TRACE_FILE_DEFAULT}" ]]; then
  echo "Error: trace node list not found: ${TRACE_FILE_DEFAULT}" >&2
  echo "exp5 currently reuses cb-simulator and requires a TRACE_FILE node list." >&2
  exit 1
fi

ALGO_LC="$(echo "${ALGO}" | tr '[:upper:]' '[:lower:]')"
WORKLOAD_LC="$(echo "${WORKLOAD}" | tr '[:upper:]' '[:lower:]')"
RECEIVER_STRATEGY_LC="$(echo "${RECEIVER_STRATEGY}" | tr '[:upper:]' '[:lower:]')"

case "${RECEIVER_STRATEGY_LC}" in
  rr)
    RECEIVER_STRATEGY_CFG="RR"
    ;;
  srpt)
    RECEIVER_STRATEGY_CFG="SRPT"
    ;;
  *)
    echo "Error: unsupported --receiver-strategy '${RECEIVER_STRATEGY}'. Use rr or srpt" >&2
    exit 1
    ;;
esac

BASE_CFG=""
ENABLE_PFC=""
ENABLE_CREDITBOUNCER=""
CC_MODE=""
CB_CC_ENABLE=""
CB_FC_ENABLE=""
CB_SWITCH_ENABLE=""

case "${ALGO_LC}" in
  dpdq-ccfc)
    BASE_CFG="${ROOT_DIR}/config/config-dpdq-ccfc.txt"
    ENABLE_PFC="0"
    ENABLE_CREDITBOUNCER="1"
    CC_MODE="0"
    CB_CC_ENABLE="1"
    CB_FC_ENABLE="1"
    CB_SWITCH_ENABLE="1"
    ;;
  dpdq-fc)
    BASE_CFG="${ROOT_DIR}/config/config-dpdq-fc.txt"
    ENABLE_PFC="0"
    ENABLE_CREDITBOUNCER="1"
    CC_MODE="0"
    CB_CC_ENABLE="0"
    CB_FC_ENABLE="1"
    CB_SWITCH_ENABLE="1"
    ;;
  pfc)
    BASE_CFG="${ROOT_DIR}/config/config-pfc.txt"
    ENABLE_PFC="1"
    ENABLE_CREDITBOUNCER="0"
    CC_MODE="0"
    CB_CC_ENABLE="0"
    CB_FC_ENABLE="0"
    CB_SWITCH_ENABLE="0"
    ;;
  pfc-dcqcn)
    BASE_CFG="${ROOT_DIR}/config/config-pfc-dcqcn.txt"
    ENABLE_PFC="1"
    ENABLE_CREDITBOUNCER="0"
    CC_MODE="1"
    CB_CC_ENABLE="0"
    CB_FC_ENABLE="0"
    CB_SWITCH_ENABLE="0"
    ;;
  pfc-hpcc)
    BASE_CFG="${ROOT_DIR}/config/config-pfc-hpcc.txt"
    ENABLE_PFC="1"
    ENABLE_CREDITBOUNCER="0"
    CC_MODE="3"
    CB_CC_ENABLE="0"
    CB_FC_ENABLE="0"
    CB_SWITCH_ENABLE="0"
    ;;
  *)
    echo "Error: unsupported algo '${ALGO}'. Use one of: dpdq-ccfc, dpdq-fc, pfc, pfc-dcqcn, pfc-hpcc" >&2
    exit 1
    ;;
esac

if [[ ! -f "${BASE_CFG}" ]]; then
  echo "Error: base config not found: ${BASE_CFG}" >&2
  exit 1
fi

WORKLOAD_FILE=""
case "${WORKLOAD_LC}" in
  websearch)
    WORKLOAD_FILE="${ROOT_DIR}/workloads/Websearch.txt"
    ;;
  hadoop)
    WORKLOAD_FILE="${ROOT_DIR}/workloads/Facebook_HadoopDist_All_MTU1500.txt"
    ;;
  *)
    echo "Error: unsupported workload '${WORKLOAD}'. Use one of: websearch, hadoop" >&2
    exit 1
    ;;
esac

TS="$(date +%Y%m%d-%H%M%S)"
OUT_TAG="exp5-${ALGO_LC}-${WORKLOAD_LC}-load${LOAD//./p}-${TS}"
MIX_BASE_DIR="${MIX_ROOT}"
if [[ -n "${MIX_SUBDIR}" ]]; then
  MIX_BASE_DIR="${MIX_ROOT}/${MIX_SUBDIR}"
fi
MIX_DIR="${MIX_BASE_DIR}/${OUT_TAG}"
RUN_CFG="${CFG_DIR}/config-${OUT_TAG}.txt"
RUN_CFG_LOCAL="${RUN_CFG}"

mkdir -p "${CFG_DIR}" "${LOG_DIR}" "${MIX_BASE_DIR}" "${MIX_DIR}" "${STATE_DIR}"
cp "${BASE_CFG}" "${RUN_CFG}"

set_config_key() {
  local cfg="$1"
  local key="$2"
  local value="$3"
  awk -v k="${key}" -v v="${value}" '
    BEGIN { done=0 }
    {
      if (!done && $1 == k) {
        print k " " v;
        done=1;
      } else {
        print $0;
      }
    }
    END {
      if (!done) print k " " v;
    }
  ' "${cfg}" > "${cfg}.tmp"
  mv "${cfg}.tmp" "${cfg}"
}

set_config_key "${RUN_CFG}" "CC_MODE" "${CC_MODE}"
set_config_key "${RUN_CFG}" "ENABLE_PFC" "${ENABLE_PFC}"
set_config_key "${RUN_CFG}" "ENABLE_CREDITBOUNCER" "${ENABLE_CREDITBOUNCER}"
set_config_key "${RUN_CFG}" "CREDITBOUNCER_CC_ENABLE" "${CB_CC_ENABLE}"
set_config_key "${RUN_CFG}" "CREDITBOUNCER_FC_ENABLE" "${CB_FC_ENABLE}"
set_config_key "${RUN_CFG}" "CREDITBOUNCER_GRANT_SCHED_POLICY" "${RECEIVER_STRATEGY_CFG}"
set_config_key "${RUN_CFG}" "CREDITBOUNCER_SWITCH_CB_ENABLE" "${CB_SWITCH_ENABLE}"
set_config_key "${RUN_CFG}" "LOAD" "${LOAD}"
set_config_key "${RUN_CFG}" "APP_START_TIME" "${APP_START_TIME}"
set_config_key "${RUN_CFG}" "APP_STOP_TIME" "${APP_STOP_TIME}"
set_config_key "${RUN_CFG}" "ENABLE_FOREGROUND_INCAST" "${ENABLE_FOREGROUND_INCAST}"
set_config_key "${RUN_CFG}" "FOREGROUND_INCAST_START_TIME" "${FOREGROUND_INCAST_START_TIME}"
set_config_key "${RUN_CFG}" "HPCC_WORKLOAD" "${WORKLOAD_FILE}"
set_config_key "${RUN_CFG}" "TOPOLOGY_FILE" "${ROOT_DIR}/config/topology96-ll.txt"
set_config_key "${RUN_CFG}" "TRACE_FILE" "${TRACE_FILE_DEFAULT}"
set_config_key "${RUN_CFG}" "TRACE_OUTPUT_FILE" "${MIX_DIR}/${ALGO_LC}-${WORKLOAD_LC}.tr"
set_config_key "${RUN_CFG}" "FCT_OUTPUT_FILE" "${MIX_DIR}/${ALGO_LC}-${WORKLOAD_LC}-fct.txt"
set_config_key "${RUN_CFG}" "TIMEOUT_OUTPUT_FILE" "${MIX_DIR}/${ALGO_LC}-${WORKLOAD_LC}-timeout.txt"
set_config_key "${RUN_CFG}" "PACKET_LOSS_OUTPUT_FILE" "${MIX_DIR}/${ALGO_LC}-${WORKLOAD_LC}-packet-loss.txt"
set_config_key "${RUN_CFG}" "STANDALONE_FCT_MODE" "1"

if [[ "${ENABLE_PFC}" == "1" ]]; then
  set_config_key "${RUN_CFG}" "PFC_OUTPUT_FILE" "${MIX_DIR}/${ALGO_LC}-${WORKLOAD_LC}-pfc.txt"
  set_config_key "${RUN_CFG}" "PAUSE_INTERVALS_OUTPUT_FILE" "${MIX_DIR}/${ALGO_LC}-${WORKLOAD_LC}-pause-intervals.txt"
  set_config_key "${RUN_CFG}" "BOUNCED_OUTPUT_FILE" "/dev/null"
  set_config_key "${RUN_CFG}" "XOFF_INTERVALS_OUTPUT_FILE" "/dev/null"
else
  set_config_key "${RUN_CFG}" "PFC_OUTPUT_FILE" "/dev/null"
  set_config_key "${RUN_CFG}" "PAUSE_INTERVALS_OUTPUT_FILE" "/dev/null"
  set_config_key "${RUN_CFG}" "BOUNCED_OUTPUT_FILE" "${MIX_DIR}/${ALGO_LC}-${WORKLOAD_LC}-bounced.txt"
  set_config_key "${RUN_CFG}" "XOFF_INTERVALS_OUTPUT_FILE" "${MIX_DIR}/${ALGO_LC}-${WORKLOAD_LC}-xoff-intervals.txt"
fi

echo "[run] algo=${ALGO_LC} workload=${WORKLOAD_LC} load=${LOAD} receiver_strategy=${RECEIVER_STRATEGY_CFG}"
echo "[run] cfg=${RUN_CFG}"
echo "[run] base_cfg=${BASE_CFG}"
echo "[run] out=${MIX_DIR}"
echo "[run] bin=${SIM_BIN}"

if [[ ${DRY_RUN} -eq 1 ]]; then
  echo "[done] dry-run enabled, simulator not started"
  exit 0
fi

SIM_TIMEOUT="${SIM_TIMEOUT:-0}"
RUN_CMD=(env "SIM_BIN=${SIM_BIN}" "${RUNNER}" --in "${RUN_CFG_LOCAL}")
if [[ "${SIM_TIMEOUT}" != "0" ]]; then
  RUN_CMD=(timeout --signal=TERM "${SIM_TIMEOUT}" "${RUN_CMD[@]}")
fi
PID_FILE="${STATE_DIR}/${OUT_TAG}.pid"
LATEST_PID_FILE="${STATE_DIR}/latest.pid"
STATUS_FILE="${STATE_DIR}/${OUT_TAG}.status"
RUN_LOG="${LOG_DIR}/${OUT_TAG}.log"

if command -v ionice >/dev/null 2>&1; then
  RUN_CMD=(ionice -c2 -n7 "${RUN_CMD[@]}")
fi
if command -v nice >/dev/null 2>&1; then
  RUN_CMD=(nice -n 10 "${RUN_CMD[@]}")
fi

HOST_GUARD_ENABLE="${HOST_GUARD_ENABLE:-1}"
HOST_GUARD_INTERVAL_SEC="${HOST_GUARD_INTERVAL_SEC:-5}"
HOST_GUARD_MIN_MEM_MB="${HOST_GUARD_MIN_MEM_MB:-256}"
HOST_GUARD_MIN_SWAPFREE_MB="${HOST_GUARD_MIN_SWAPFREE_MB:-256}"
HOST_GUARD_MEM_PSI_SOME_AVG10_MAX="${HOST_GUARD_MEM_PSI_SOME_AVG10_MAX:-20.0}"
HOST_GUARD_MEM_PSI_FULL_AVG10_MAX="${HOST_GUARD_MEM_PSI_FULL_AVG10_MAX:-5.0}"
HOST_GUARD_BREACH_LIMIT="${HOST_GUARD_BREACH_LIMIT:-24}"

sim_pid=""
watchdog_pid=""
cleanup() {
  if [[ -n "${watchdog_pid}" ]] && kill -0 "${watchdog_pid}" 2>/dev/null; then
    kill "${watchdog_pid}" 2>/dev/null || true
    wait "${watchdog_pid}" 2>/dev/null || true
  fi
  rm -f "${PID_FILE}" "${LATEST_PID_FILE}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

host_guard() {
  local pid="$1"
  local log_file="$2"
  local breach=0

  while kill -0 "${pid}" 2>/dev/null; do
    sleep "${HOST_GUARD_INTERVAL_SEC}"
    kill -0 "${pid}" 2>/dev/null || break

    local mem_avail_kb swap_free_kb mem_avail_mb swap_free_mb
    local mem_psi_some_avg10 mem_psi_full_avg10
    local low_mem low_swap psi_some_bad psi_full_bad
    mem_avail_kb="$(awk '/MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    swap_free_kb="$(awk '/SwapFree:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    mem_avail_mb="$((mem_avail_kb / 1024))"
    swap_free_mb="$((swap_free_kb / 1024))"
    mem_psi_some_avg10="$(awk '/^some / {for(i=1;i<=NF;i++) if($i ~ /^avg10=/){split($i,a,"="); print a[2]}}' /proc/pressure/memory 2>/dev/null || echo 0)"
    mem_psi_full_avg10="$(awk '/^full / {for(i=1;i<=NF;i++) if($i ~ /^avg10=/){split($i,a,"="); print a[2]}}' /proc/pressure/memory 2>/dev/null || echo 0)"

    low_mem=0
    if (( mem_avail_mb < HOST_GUARD_MIN_MEM_MB )); then
      low_mem=1
    fi
    low_swap=0
    if (( swap_free_mb < HOST_GUARD_MIN_SWAPFREE_MB )); then
      low_swap=1
    fi
    psi_some_bad="$(awk -v v="${mem_psi_some_avg10}" -v t="${HOST_GUARD_MEM_PSI_SOME_AVG10_MAX}" 'BEGIN{print (v>t)?1:0}')"
    psi_full_bad="$(awk -v v="${mem_psi_full_avg10}" -v t="${HOST_GUARD_MEM_PSI_FULL_AVG10_MAX}" 'BEGIN{print (v>t)?1:0}')"

    if [[ ( "${low_mem}" -eq 1 && "${low_swap}" -eq 1 ) || "${psi_some_bad}" -eq 1 || "${psi_full_bad}" -eq 1 ]]; then
      breach=$((breach + 1))
      echo "[guard] breach=${breach}/${HOST_GUARD_BREACH_LIMIT} mem_avail_mb=${mem_avail_mb} swap_free_mb=${swap_free_mb} mem_psi_some_avg10=${mem_psi_some_avg10} mem_psi_full_avg10=${mem_psi_full_avg10}" >> "${log_file}"
    else
      breach=0
    fi

    if (( breach >= HOST_GUARD_BREACH_LIMIT )); then
      echo "[guard] host pressure too high, terminating pid=${pid}" >> "${log_file}"
      {
        echo "status=guard_stop"
        echo "time=$(date --iso-8601=seconds)"
        echo "pid=${pid}"
        echo "mem_avail_mb=${mem_avail_mb}"
        echo "swap_free_mb=${swap_free_mb}"
        echo "mem_psi_some_avg10=${mem_psi_some_avg10}"
        echo "mem_psi_full_avg10=${mem_psi_full_avg10}"
      } > "${STATUS_FILE}"
      kill -TERM "${pid}" 2>/dev/null || true
      sleep 10
      kill -KILL "${pid}" 2>/dev/null || true
      break
    fi
  done
}

echo "[run] log=${RUN_LOG}"
echo "[run] pid_file=${PID_FILE}"
echo "[run] sim_timeout=${SIM_TIMEOUT} (0 means no fixed timeout)"

set +e
"${RUN_CMD[@]}" > "${RUN_LOG}" 2>&1 &
sim_pid=$!
echo "${sim_pid}" > "${PID_FILE}"
cp "${PID_FILE}" "${LATEST_PID_FILE}"

if [[ "${HOST_GUARD_ENABLE}" == "1" ]]; then
  host_guard "${sim_pid}" "${RUN_LOG}" &
  watchdog_pid=$!
fi

wait "${sim_pid}"
rc=$?
set -e
if [[ ${rc} -eq 124 ]]; then
  echo "[warn] run timeout reached (${SIM_TIMEOUT}), stopped to protect host stability" >&2
  exit 124
fi
if [[ ${rc} -ne 0 ]]; then
  exit "${rc}"
fi
echo "[done] log=${RUN_LOG}"
