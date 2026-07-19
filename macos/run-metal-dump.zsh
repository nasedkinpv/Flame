#!/bin/zsh
# Texture-dump session: same flow as run-metal-game.zsh, but the Metal host
# runs with DK2_TEXTURE_DUMP enabled (and HD replacement active if
# textures-hd exists). Walk around the level, then feed the dump to
# tools/upscale_textures.py and tools/curate_textures.py.
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
readonly SHADOW_LEVEL="${DK2_SHADOW_LEVEL:-3}"
readonly IMPORTER="${SCRIPT_DIR}/import-original-game.zsh"

if (( $# == 0 )); then
  LEVEL="${DK2_LEVEL:-level1}"
elif (( $# == 2 )) && [[ "$1" == '-LEVEL' ]]; then
  LEVEL="$2"
else
  print -u2 -- "usage: ${0:t} [-LEVEL levelN]"
  exit 2
fi
readonly LEVEL
readonly DUMP_DIR="${DK2_TEXTURE_DUMP:-${HOME}/Library/Application Support/Dungeon Keeper 2 Flame/texture-dump}"

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
if [[ ! -f "${GAME_DIR}/DKII-DX.exe" ]]; then
  DK2_WINE_BIN="${WINE}" DK2_METAL_PREFIX="${PREFIX}" "${IMPORTER}"
fi
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
      -skip-launcher -game-res=1600x1200 -LEVEL "${LEVEL}" \
      -Q -NoMovies -DisableGamma -NoSound -Shadows "${SHADOW_LEVEL}" \
      -gog:video:HighRes=true -gog:video:RealFullscreen=false -gog:video:Vwait=0 \
      -gog:misc:CpuIdle=1 -gog:misc:RestoreMode=1
) >>"${LOG_FILE}" 2>&1 &

for attempt in {1..2400}; do
  current_frame="$(bridge_frame)"
  [[ "${current_frame}" != 0 && "${current_frame}" != "${initial_frame}" ]] && break
  /bin/sleep 0.05
done
[[ "$(bridge_frame)" != 0 && "$(bridge_frame)" != "${initial_frame}" ]] ||
  fail "DK2 did not produce a Metal frame; see ${LOG_FILE}"

print -- "texture dump -> ${DUMP_DIR}"
DK2_TEXTURE_DUMP="${DUMP_DIR}" "${APP_EXECUTABLE}" "--bridge-file=${BRIDGE_FILE}"
