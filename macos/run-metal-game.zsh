#!/bin/zsh
# Launch the Metal host as an app (LaunchServices) and let IT spawn the wine
# chain via macos/dk2-runner.zsh, so lifecycle and termination stay tied
# together. Game Mode is intentionally disabled for this two-process pipeline.
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly REPO_ROOT="${SCRIPT_DIR:h}"
readonly APP="${DK2_METAL_APP_BUNDLE:-${SCRIPT_DIR}/native/build/Dungeon Keeper II.app}"
readonly RUNNER="${SCRIPT_DIR}/dk2-runner.zsh"
readonly PREFIX="${DK2_METAL_PREFIX:-${HOME}/Library/Application Support/Dungeon Keeper II/prefix}"
readonly BRIDGE_FILE="${PREFIX}/drive_c/dk2-metal/frame.bin"
readonly SHADOW_LEVEL="${DK2_SHADOW_LEVEL:-3}"

if (( $# == 0 )); then
  LEVEL=''
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
# Apple's Metal HUD (FPS/frame-time overlay). Reads its own env var at first
# Metal API use, so this only works set BEFORE the process starts using
# Metal - `open` does not propagate shell env to the launched app, which is
# why this goes through --runner-env (DK2Metal calls setenv() on itself for
# every K=V here, at the very top of main(), before NSApplication even
# starts). Toggle: DK2_METAL_HUD=0 to disable.
HUD_ARG=()
[[ "${DK2_METAL_HUD:-1}" == 1 ]] && HUD_ARG=("--runner-env=MTL_HUD_ENABLED=1")
# wip: throwaway passthrough for the heightfield-guard-bypass experiment
# (2026-07-24) -- same open/--runner-env limitation as MTL_HUD_ENABLED above.
# Remove once the terrain-LOD-on-zoom investigation is resolved.
BYPASS_ARG=()
[[ -n "${DK2_HEIGHTFIELD_BYPASS_GUARDS:-}" ]] && BYPASS_ARG=("--runner-env=DK2_HEIGHTFIELD_BYPASS_GUARDS=${DK2_HEIGHTFIELD_BYPASS_GUARDS}")
exec open -W -n "${APP}" --args \
  "${FS_ARG[@]}" \
  "${HUD_ARG[@]}" \
  "${BYPASS_ARG[@]}" \
  "--bridge-file=${BRIDGE_FILE}" \
  "--game-runner=${RUNNER}" \
  "--runner-env=DK2_LEVEL=${LEVEL}" \
  "--runner-env=DK2_SHADOW_LEVEL=${SHADOW_LEVEL}"
