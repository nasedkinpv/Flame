#!/bin/zsh
# Launch the Metal host as an app (LaunchServices) and let IT spawn the wine
# chain via macos/dk2-wine-runner.zsh, so lifecycle and termination stay tied
# together. Game Mode is intentionally disabled for this two-process pipeline.
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly REPO_ROOT="${SCRIPT_DIR:h}"
readonly APP="${DK2_METAL_APP_BUNDLE:-${SCRIPT_DIR}/native/build/Dungeon Keeper II.app}"
readonly RUNNER="${SCRIPT_DIR}/dk2-wine-runner.zsh"
readonly PREFIX="${DK2_METAL_PREFIX:-${HOME}/Library/Application Support/Dungeon Keeper II Metal/prefix}"
readonly BRIDGE_FILE="${PREFIX}/drive_c/dk2-metal/frame.bin"
readonly SHADOW_LEVEL="${DK2_SHADOW_LEVEL:-3}"

if (( $# == 0 )); then
  LEVEL="${DK2_LEVEL:-level1}"
elif (( $# == 2 )) && [[ "$1" == '-LEVEL' ]]; then
  LEVEL="$2"
else
  print -u2 -- "usage: ${0:t} [-LEVEL levelN]"
  exit 2
fi
readonly LEVEL

fail() {
  print -u2 -- "error: $*"
  exit 1
}

[[ -d "${APP}" ]] || fail "build the Metal host with macos/build-metal-host.zsh"
[[ -x "${RUNNER}" ]] || fail "missing ${RUNNER}"
/bin/mkdir -p "${BRIDGE_FILE:h}"

FS_ARG=()
[[ "${DK2_FULLSCREEN:-1}" == 1 ]] && FS_ARG=(--fullscreen)
exec open -W -n "${APP}" --args \
  "${FS_ARG[@]}" \
  "--bridge-file=${BRIDGE_FILE}" \
  "--game-runner=${RUNNER}" \
  "--runner-env=DK2_LEVEL=${LEVEL}" \
  "--runner-env=DK2_SHADOW_LEVEL=${SHADOW_LEVEL}"
