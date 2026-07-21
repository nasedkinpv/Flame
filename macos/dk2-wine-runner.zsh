#!/bin/zsh
# Shared wine runner: launched BY the Metal host (--game-runner=) so lifecycle
# and termination stay tied to the native app. Game Mode is intentionally off.
# Configuration comes from the environment (passed via --runner-env=):
#   DK2_LEVEL, DK2_SHADOW_LEVEL, DK2_WINE_BIN, DK2_METAL_PREFIX
# Stays alive until every wine process exits (wineserver -w), which lets the
# host quit when the game does.
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly REPO_ROOT="${SCRIPT_DIR:h}"
readonly WINE="${DK2_WINE_BIN:-${REPO_ROOT}/.cache/wine-stable-11.0_1/Contents/Resources/wine/bin/wine}"
readonly WINESERVER="${WINE:h}/wineserver"
readonly PREFIX="${DK2_METAL_PREFIX:-${HOME}/Library/Application Support/Dungeon Keeper II Metal/prefix}"
readonly GAME_DIR="${PREFIX}/drive_c/GOG Games/Dungeon Keeper 2"
readonly LOG_DIR="${HOME}/Library/Logs/Dungeon Keeper II Metal"
readonly LOG_FILE="${LOG_DIR}/game.log"
readonly SHADOW_LEVEL="${DK2_SHADOW_LEVEL:-3}"
readonly LEVEL="${DK2_LEVEL:-}"
# Widescreen needs the DirectDraw mode list to expose non-4:3 modes first;
# until then the safe default is the largest mode the bridge enumerates.
readonly GAME_RES="${DK2_GAME_RES:-1600x1200}"
LEVEL_ARGS=()
[[ -n "${LEVEL}" ]] && LEVEL_ARGS=(-LEVEL "${LEVEL}" -Q)

fail() {
  print -u2 -- "error: $*"
  exit 1
}

[[ -x "${WINE}" && -x "${WINESERVER}" ]] || fail "Wine 11 runtime is missing"
[[ -f "${GAME_DIR}/DKII-DX.exe" && -f "${GAME_DIR}/patch.dll" ]] || fail "isolated DK2 installation is missing"

/bin/mkdir -p "${LOG_DIR}"
# kill any wineserver left over from terminal sessions: a game forked by a
# stale wineserver would escape the app lifecycle and retain old bridge state
env WINEPREFIX="${PREFIX}" "${WINESERVER}" -k >/dev/null 2>&1 || true
env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w >/dev/null 2>&1 || true

cd "${GAME_DIR}" || fail "game directory is missing"
env -i \
  PATH='/usr/bin:/bin' \
  USER="${USER}" LOGNAME="${LOGNAME:-${USER}}" LANG='en_US.UTF-8' TMPDIR='/tmp' \
  WINEPREFIX="${PREFIX}" WINEDEBUG="${DK2_WINEDEBUG:--all}" \
  WINEDLLOVERRIDES='ddraw,d3dimm,dinput=b;winedbg.exe=d' \
  DK2_METAL_BRIDGE_FILE='C:\dk2-metal\frame.bin' \
  MVK_CONFIG_LOG_LEVEL='0' \
  "${WINE}" start.exe /exec 'C:\GOG Games\Dungeon Keeper 2\DKII-DX.exe' \
    -skip-launcher -game-res="${GAME_RES}" "${LEVEL_ARGS[@]}" \
    -NoMovies -DisableGamma -Sound -Shadows "${SHADOW_LEVEL}" \
    -gog:video:HighRes=true -gog:video:RealFullscreen=false -gog:video:Vwait=0 \
    -gog:misc:CpuIdle=1 -gog:misc:RestoreMode=1 >>"${LOG_FILE}" 2>&1

exec env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w
