#!/bin/zsh
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly APP_EXECUTABLE="${SCRIPT_DIR}/native/build/Dungeon Keeper II.app/Contents/MacOS/DK2Metal"
readonly PRODUCER="${SCRIPT_DIR}/native/build/DK2BridgeProducer"
readonly TEST_ROOT="$(/usr/bin/mktemp -d /tmp/dk2-metal-test.XXXXXX)"
readonly BRIDGE_FILE="${TEST_ROOT}/frame.bin"
readonly LOG_FILE="${TEST_ROOT}/host.log"
readonly PACK_DIR="${TEST_ROOT}/resource-pack"
host_pid=""

cleanup() {
  if [[ -n "${host_pid}" ]] && /bin/kill -0 "${host_pid}" 2>/dev/null; then
    /bin/kill "${host_pid}" 2>/dev/null || true
  fi
  [[ "${TEST_ROOT}" == /tmp/dk2-metal-test.* ]] && /bin/rm -rf -- "${TEST_ROOT}"
}
trap cleanup EXIT

"${SCRIPT_DIR}/build-metal-host.zsh" >/dev/null
/bin/mkdir -p "${PACK_DIR}"
# 1x1 opaque red PNG used by BridgeProducer's SelfTestAtlas map.
print -n -- 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL8WQAAAABJRU5ErkJggg==' | \
  /usr/bin/base64 -D > "${PACK_DIR}/SelfTestAtlas.png"
env MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 DK2_BLOOM=1 DK2_METAL_SHADOWS=1 \
  DK2_RESOURCE_PACK_DIR="${PACK_DIR}" \
  "${APP_EXECUTABLE}" \
  --self-test-frames=120 \
  --bridge-self-test \
  "--bridge-file=${BRIDGE_FILE}" >"${LOG_FILE}" 2>&1 &
host_pid=$!

for attempt in {1..2000}; do
  [[ -f "${BRIDGE_FILE}" ]] && break
  /bin/sleep 0.02
done
[[ -f "${BRIDGE_FILE}" ]]
"${PRODUCER}" "${BRIDGE_FILE}"
wait "${host_pid}"
host_pid=""

/usr/bin/grep -q "Bridge accepted frame 1" "${LOG_FILE}"
/usr/bin/grep -q "DK2 resource pack: active, first page composed" "${LOG_FILE}"
/usr/bin/grep -q "SELF-TEST PASS" "${LOG_FILE}"
print -- "METAL BRIDGE SELF-TEST PASS"
