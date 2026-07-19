#!/bin/zsh
set -u

readonly CONTENTS="${0:A:h:h}"
readonly RESOURCES="${CONTENTS}/Resources"
readonly WINE="${RESOURCES}/wine/bin/wine"
readonly WINESERVER="${RESOURCES}/wine/bin/wineserver"
readonly IMPORTER="${RESOURCES}/import-original-game"
readonly PREFIX="${HOME}/Library/Application Support/Dungeon Keeper II Metal/prefix"
readonly GAME_DIR="${PREFIX}/drive_c/GOG Games/Dungeon Keeper 2"
readonly BRIDGE_FILE="${PREFIX}/drive_c/dk2-metal/frame.bin"
readonly LOG_DIR="${HOME}/Library/Logs/Dungeon Keeper II Metal"
readonly LOG_FILE="${LOG_DIR}/game.log"
readonly SHADOW_LEVEL="${DK2_SHADOW_LEVEL:-3}"

show_error() {
  /usr/bin/osascript -e "display alert \"Dungeon Keeper II\" message \"$1\" as critical" >/dev/null 2>&1 || true
}

bridge_frame() {
  [[ -f "${BRIDGE_FILE}" ]] || { print 0; return; }
  /usr/bin/od -An -j20 -N4 -tu4 "${BRIDGE_FILE}" | /usr/bin/tr -d ' '
}

cleanup() {
  env WINEPREFIX="${PREFIX}" "${WINESERVER}" -k >/dev/null 2>&1 || true
}

sync_flame_payload() {
  local payload="${RESOURCES}/Flame"
  [[ -f "${payload}/PATCH.dll" &&
     -f "${payload}/flame/Flame.dll" &&
     -f "${payload}/flame/DKII.dll" ]] || {
    show_error "The bundled Flame payload is incomplete. Download the app again."
    return 1
  }
  /bin/mkdir -p "${GAME_DIR}/flame"
  /bin/cp -p "${payload}/PATCH.dll" "${GAME_DIR}/PATCH.dll"
  /bin/cp -p "${payload}/flame/Flame.dll" "${GAME_DIR}/Flame.dll"
  /bin/cp -p "${payload}/flame/DKII.dll" "${GAME_DIR}/DKII.dll"
  /bin/cp -p "${payload}/flame/Flame.dll" "${GAME_DIR}/flame/Flame.dll"
  /bin/cp -p "${payload}/flame/DKII.dll" "${GAME_DIR}/flame/DKII.dll"
}
trap cleanup EXIT INT TERM HUP

/bin/mkdir -p "${LOG_DIR}"
: >| "${LOG_FILE}"
if [[ ! -x "${WINE}" || ! -x "${WINESERVER}" ]]; then
  show_error "The application bundle is incomplete. Download it again."
  exit 1
fi

if [[ ! -f "${GAME_DIR}/DKII-DX.exe" ]]; then
  DK2_WINE_BIN="${WINE}" \
  DK2_METAL_PREFIX="${PREFIX}" \
  DK2_FLAME_PAYLOAD="${RESOURCES}/Flame" \
  DK2_IMPORT_GUI=1 \
    "${IMPORTER}" >>"${LOG_FILE}" 2>&1
  import_status=$?
  if (( import_status != 0 )); then
    (( import_status == 2 )) || show_error "Import failed. See ${LOG_FILE}."
    exit ${import_status}
  fi
fi

cleanup
env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w >/dev/null 2>&1 || true
sync_flame_payload || exit 1
/bin/mkdir -p "${BRIDGE_FILE:h}"
initial_frame="$(bridge_frame)"
game_mode_args=()
if [[ -n "${DK2_LEVEL:-}" ]]; then
  game_mode_args=(-LEVEL "${DK2_LEVEL}" -Q)
fi
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
      -skip-launcher -game-res=1600x1200 \
      "${game_mode_args[@]}" \
      -NoMovies -NoSound -Shadows "${SHADOW_LEVEL}" \
      -enablebumpmapping -enablebumpluminance -32biteverything -disablegamma \
      -gog:video:HighRes=true -gog:video:RealFullscreen=false -gog:video:Vwait=0 \
      -gog:misc:CpuIdle=1 -gog:misc:RestoreMode=1
) >>"${LOG_FILE}" 2>&1 &

for attempt in {1..600}; do
  current_frame="$(bridge_frame)"
  [[ "${current_frame}" != 0 && "${current_frame}" != "${initial_frame}" ]] && break
  /bin/sleep 0.05
done
if [[ "$(bridge_frame)" == 0 || "$(bridge_frame)" == "${initial_frame}" ]]; then
  show_error "The game didn't produce a frame. See ${LOG_FILE}."
  exit 1
fi

env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w >>"${LOG_FILE}" 2>&1
