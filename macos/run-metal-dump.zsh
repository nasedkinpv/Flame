#!/bin/zsh
# Texture-dump session: run-metal-game.zsh flow with DK2_TEXTURE_DUMP enabled
# on the host (passed via --runner-env, which the host also applies to its own
# environment before the renderer starts).
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly APP="${DK2_METAL_APP_BUNDLE:-${SCRIPT_DIR}/native/build/Dungeon Keeper II.app}"
readonly RUNNER="${SCRIPT_DIR}/dk2-wine-runner.zsh"
readonly PREFIX="${DK2_METAL_PREFIX:-${HOME}/Library/Application Support/Dungeon Keeper II Metal/prefix}"
readonly BRIDGE_FILE="${PREFIX}/drive_c/dk2-metal/frame.bin"
readonly SHADOW_LEVEL="${DK2_SHADOW_LEVEL:-3}"
readonly DUMP_DIR="${DK2_TEXTURE_DUMP:-${HOME}/Library/Application Support/Dungeon Keeper 2 Flame/texture-dump}"

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

print -- "texture dump -> ${DUMP_DIR}"
exec open -W -n "${APP}" --args \
  "--bridge-file=${BRIDGE_FILE}" \
  "--game-runner=${RUNNER}" \
  "--runner-env=DK2_LEVEL=${LEVEL}" \
  "--runner-env=DK2_SHADOW_LEVEL=${SHADOW_LEVEL}" \
  "--runner-env=DK2_TEXTURE_DUMP=${DUMP_DIR}"
