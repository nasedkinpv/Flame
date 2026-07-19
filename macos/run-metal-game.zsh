#!/bin/zsh
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly REPO_ROOT="${SCRIPT_DIR:h}"
readonly APP_EXECUTABLE="${DK2_METAL_APP:-${SCRIPT_DIR}/native/build/Dungeon Keeper II.app/Contents/MacOS/DK2Metal}"
readonly WINE="${DK2_WINE_BIN:-${REPO_ROOT}/.cache/wine-stable-11.0_1/Contents/Resources/wine/bin/wine}"
readonly WINESERVER="${WINE:h}/wineserver"
readonly PREFIX="${DK2_METAL_PREFIX:-${HOME}/Library/Application Support/Dungeon Keeper II Metal/prefix}"
readonly GAME_DIR="${PREFIX}/drive_c/GOG Games/Dungeon Keeper 2"
readonly BRIDGE_FILE="${PREFIX}/drive_c/dk2-metal/frame.bin"
readonly LOG_DIR="${HOME}/Library/Logs/Dungeon Keeper II Metal"
readonly LOG_FILE="${LOG_DIR}/game.log"
readonly SHADOW_LEVEL="${DK2_SHADOW_LEVEL:-1}"

fail() {
  print -u2 -- "error: $*"
  exit 1
}

bridge_frame() {
  [[ -f "${BRIDGE_FILE}" ]] || { print 0; return; }
  /usr/bin/od -An -j20 -N4 -tu4 "${BRIDGE_FILE}" | /usr/bin/tr -d ' '
}

cleanup() {
  env WINEPREFIX="${PREFIX}" "${WINESERVER}" -k >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM HUP

[[ -x "${APP_EXECUTABLE}" ]] || fail "build the Metal host with macos/build-metal-host.zsh"
[[ -x "${WINE}" && -x "${WINESERVER}" ]] || fail "Wine 11 runtime is missing"
[[ -f "${GAME_DIR}/DKII-DX.exe" && -f "${GAME_DIR}/patch.dll" ]] || fail "isolated DK2 installation is missing"

/bin/mkdir -p "${BRIDGE_FILE:h}" "${LOG_DIR}"
cleanup
env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w >/dev/null 2>&1 || true
initial_frame="$(bridge_frame)"

(
  cd "${GAME_DIR}" || exit 1
  env -i \
    PATH='/usr/bin:/bin' \
    USER="${USER}" LOGNAME="${LOGNAME:-${USER}}" LANG='en_US.UTF-8' TMPDIR='/tmp' \
    WINEPREFIX="${PREFIX}" WINEDEBUG='-all' \
    WINEDLLOVERRIDES='ddraw,d3dimm,dinput=b;winedbg.exe=d' \
    DK2_METAL_BRIDGE_FILE='C:\dk2-metal\frame.bin' \
    MVK_CONFIG_LOG_LEVEL='0' \
    "${WINE}" start.exe /exec 'C:\GOG Games\Dungeon Keeper 2\DKII-DX.exe' \
      -skip-launcher -game-res=1024x768 -Level=level1 \
      -Q -NoMovies -DisableGamma -NoSound -Shadows "${SHADOW_LEVEL}" \
      -gog:video:RealFullscreen=false -gog:video:Vwait=0 \
      -gog:misc:CpuIdle=1 -gog:misc:RestoreMode=1
) >>"${LOG_FILE}" 2>&1 &

for attempt in {1..200}; do
  current_frame="$(bridge_frame)"
  [[ "${current_frame}" != 0 && "${current_frame}" != "${initial_frame}" ]] && break
  /bin/sleep 0.05
done
[[ "$(bridge_frame)" != 0 && "$(bridge_frame)" != "${initial_frame}" ]] ||
  fail "DK2 did not produce a Metal frame; see ${LOG_FILE}"

"${APP_EXECUTABLE}" "--bridge-file=${BRIDGE_FILE}"
