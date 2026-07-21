#!/bin/zsh
# Unified DK2 wine runner: launched BY the Metal host (--game-runner=) so
# lifecycle and termination stay tied to the native app. Game Mode is
# intentionally off.
#
# One script, two modes, auto-detected from where it lives:
#   - packaged: this script sits at Contents/Resources/dk2-game-runner inside
#     the .app bundle, next to a bundled Contents/Resources/wine runtime. It
#     runs the first-launch import flow, syncs the bundled Flametal payload
#     into the isolated install, and watches the bridge file for the first
#     frame (surfacing failures via osascript alert dialogs since there is no
#     terminal to read).
#   - dev: run straight from a repo checkout. Wine comes from .cache/, the
#     isolated install is expected to already exist (see
#     macos/import-original-game.zsh), and output goes to the log file +
#     terminal instead of alert dialogs.
#
# Configuration comes from the environment (passed via --runner-env= from the
# host, or exported directly in dev):
#   DK2_LEVEL, DK2_SHADOW_LEVEL, DK2_GAME_RES, DK2_WINE_BIN,
#   DK2_METAL_PREFIX, DK2_WINEDEBUG, DK2_HEADLESS_DDRAW
# Stays alive until every wine process exits (wineserver -w), which lets the
# host quit when the game does.
set -u

readonly SCRIPT_DIR="${0:A:h}"

if [[ -x "${SCRIPT_DIR}/wine/bin/wine" ]]; then
  readonly DK2_RUNNER_MODE='packaged'
  readonly RESOURCES="${SCRIPT_DIR}"
else
  readonly DK2_RUNNER_MODE='dev'
  readonly REPO_ROOT="${SCRIPT_DIR:h}"
fi

readonly WINE="${DK2_WINE_BIN:-$(
  if [[ "${DK2_RUNNER_MODE}" == packaged ]]; then
    print -r -- "${RESOURCES}/wine/bin/wine"
  else
    print -r -- "${REPO_ROOT}/.cache/wine-stable-11.0_1/Contents/Resources/wine/bin/wine"
  fi
)}"
readonly WINESERVER="${WINE:h}/wineserver"
readonly PREFIX="${DK2_METAL_PREFIX:-${HOME}/Library/Application Support/Dungeon Keeper II/prefix}"
readonly GAME_DIR="${PREFIX}/drive_c/GOG Games/Dungeon Keeper 2"
readonly BRIDGE_FILE="${PREFIX}/drive_c/dk2-metal/frame.bin"
readonly LOG_DIR="${HOME}/Library/Logs/Dungeon Keeper II"
readonly LOG_FILE="${LOG_DIR}/game.log"
readonly SHADOW_LEVEL="${DK2_SHADOW_LEVEL:-3}"
readonly LEVEL="${DK2_LEVEL:-}"
readonly HEADLESS_DDRAW="${DK2_HEADLESS_DDRAW:-1}"
# Widescreen needs the DirectDraw mode list to expose non-4:3 modes first;
# until then the safe default is the largest mode the bridge enumerates.
readonly GAME_RES="${DK2_GAME_RES:-1600x1200}"
LEVEL_ARGS=()
[[ -n "${LEVEL}" ]] && LEVEL_ARGS=(-LEVEL "${LEVEL}" -Q)

show_error() {
  /usr/bin/osascript -e "display alert \"Dungeon Keeper II\" message \"$1\" as critical" >/dev/null 2>&1 || true
}

fail() {
  print -u2 -- "error: $*"
  [[ "${DK2_RUNNER_MODE}" != packaged ]] || show_error "$*"
  exit 1
}

bridge_frame() {
  [[ -f "${BRIDGE_FILE}" ]] || { print 0; return; }
  /usr/bin/od -An -j20 -N4 -tu4 "${BRIDGE_FILE}" | /usr/bin/tr -d ' '
}

sync_flametal_payload() {
  local payload="${RESOURCES}/Flametal"
  [[ -f "${payload}/PATCH.dll" &&
     -f "${payload}/flametal/Flametal.dll" &&
     -f "${payload}/flametal/DKII.dll" ]] || {
    fail "The bundled Flametal payload is incomplete. Download the app again."
  }
  /bin/mkdir -p "${GAME_DIR}/flametal"
  /bin/cp -p "${payload}/PATCH.dll" "${GAME_DIR}/PATCH.dll"
  /bin/cp -p "${payload}/flametal/Flametal.dll" "${GAME_DIR}/Flametal.dll"
  /bin/cp -p "${payload}/flametal/DKII.dll" "${GAME_DIR}/DKII.dll"
  /bin/cp -p "${payload}/flametal/Flametal.dll" "${GAME_DIR}/flametal/Flametal.dll"
  /bin/cp -p "${payload}/flametal/DKII.dll" "${GAME_DIR}/flametal/DKII.dll"
}

/bin/mkdir -p "${LOG_DIR}"
[[ -x "${WINE}" && -x "${WINESERVER}" ]] || fail "Wine runtime is missing"

if [[ "${DK2_RUNNER_MODE}" == packaged ]]; then
  : >| "${LOG_FILE}"
  trap 'env WINEPREFIX="${PREFIX}" "${WINESERVER}" -k >/dev/null 2>&1 || true' EXIT INT TERM HUP

  if [[ ! -f "${GAME_DIR}/DKII-DX.exe" ]]; then
    DK2_WINE_BIN="${WINE}" \
    DK2_METAL_PREFIX="${PREFIX}" \
    DK2_FLAMETAL_PAYLOAD="${RESOURCES}/Flametal" \
    DK2_IMPORT_GUI=1 \
      "${RESOURCES}/import-original-game" >>"${LOG_FILE}" 2>&1
    import_status=$?
    if (( import_status != 0 )); then
      (( import_status == 2 )) || show_error "Import failed. See ${LOG_FILE}."
      exit ${import_status}
    fi
  fi

  env WINEPREFIX="${PREFIX}" "${WINESERVER}" -k >/dev/null 2>&1 || true
  env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w >/dev/null 2>&1 || true
  sync_flametal_payload
  /bin/mkdir -p "${BRIDGE_FILE:h}"
  initial_frame="$(bridge_frame)"
  (
    cd "${GAME_DIR}" || exit 1
    env -i \
      PATH='/usr/bin:/bin' \
      USER="${USER}" LOGNAME="${LOGNAME:-${USER}}" LANG='en_US.UTF-8' TMPDIR='/tmp' \
      WINEPREFIX="${PREFIX}" WINEDEBUG="${DK2_WINEDEBUG:--all}" \
      WINEDLLOVERRIDES='ddraw,d3dimm,dinput=b;winedbg.exe=d;mscoree,mshtml=' \
      DK2_METAL_BRIDGE_FILE='C:\dk2-metal\frame.bin' \
      DK2_HEADLESS_DDRAW="${HEADLESS_DDRAW}" \
      MVK_CONFIG_LOG_LEVEL='0' \
      "${WINE}" start.exe /exec 'C:\GOG Games\Dungeon Keeper 2\DKII-DX.exe' \
        -skip-launcher -game-res="${GAME_RES}" "${LEVEL_ARGS[@]}" \
        -NoMovies -DisableGamma -Sound -Shadows "${SHADOW_LEVEL}" \
        -gog:video:HighRes=true -gog:video:RealFullscreen=false -gog:video:Vwait=0 \
        -gog:misc:CpuIdle=1 -gog:misc:RestoreMode=1
  ) >>"${LOG_FILE}" 2>&1 &

  for attempt in {1..1800}; do
    current_frame="$(bridge_frame)"
    [[ "${current_frame}" != 0 && "${current_frame}" != "${initial_frame}" ]] && break
    /bin/sleep 0.05
  done
  if [[ "$(bridge_frame)" == 0 || "$(bridge_frame)" == "${initial_frame}" ]]; then
    show_error "The game didn't produce a frame. See ${LOG_FILE}."
    exit 1
  fi

  env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w >>"${LOG_FILE}" 2>&1
else
  set -eo pipefail
  [[ -f "${GAME_DIR}/DKII-DX.exe" && -f "${GAME_DIR}/patch.dll" ]] || fail "isolated DK2 installation is missing"

  # kill any wineserver left over from terminal sessions: a game forked by a
  # stale wineserver would escape the app lifecycle and retain old bridge state
  env WINEPREFIX="${PREFIX}" "${WINESERVER}" -k >/dev/null 2>&1 || true
  env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w >/dev/null 2>&1 || true

  cd "${GAME_DIR}" || fail "game directory is missing"
  env -i \
    PATH='/usr/bin:/bin' \
    USER="${USER}" LOGNAME="${LOGNAME:-${USER}}" LANG='en_US.UTF-8' TMPDIR='/tmp' \
    WINEPREFIX="${PREFIX}" WINEDEBUG="${DK2_WINEDEBUG:--all}" \
    WINEDLLOVERRIDES='ddraw,d3dimm,dinput=b;winedbg.exe=d;mscoree,mshtml=' \
    DK2_METAL_BRIDGE_FILE='C:\dk2-metal\frame.bin' \
    DK2_HEADLESS_DDRAW="${HEADLESS_DDRAW}" \
    MVK_CONFIG_LOG_LEVEL='0' \
    "${WINE}" start.exe /exec 'C:\GOG Games\Dungeon Keeper 2\DKII-DX.exe' \
      -skip-launcher -game-res="${GAME_RES}" "${LEVEL_ARGS[@]}" \
      -NoMovies -DisableGamma -Sound -Shadows "${SHADOW_LEVEL}" \
      -gog:video:HighRes=true -gog:video:RealFullscreen=false -gog:video:Vwait=0 \
      -gog:misc:CpuIdle=1 -gog:misc:RestoreMode=1 >>"${LOG_FILE}" 2>&1

  exec env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w
fi
