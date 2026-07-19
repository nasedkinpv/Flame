#!/bin/zsh
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly REPO_ROOT="${SCRIPT_DIR:h}"
readonly PREFIX="${DK2_METAL_PREFIX:-${HOME}/Library/Application Support/Dungeon Keeper II Metal/prefix}"
readonly GAME_PARENT="${PREFIX}/drive_c/GOG Games"
readonly GAME_DIR="${GAME_PARENT}/Dungeon Keeper 2"
readonly WINE="${DK2_WINE_BIN:-${REPO_ROOT}/.cache/wine-stable-11.0_1/Contents/Resources/wine/bin/wine}"
readonly WINESERVER="${WINE:h}/wineserver"
readonly KNOWN_GOG_EXE_SHA256='a5957e8c52b6dee80ddf6cc1d3ea5079aaaad4d11b40e0f067d34ff4157ce9bb'

show_error() {
  /usr/bin/osascript - "$1" <<'APPLESCRIPT' >/dev/null 2>&1 || true
on run argv
  display alert "Dungeon Keeper II" message (item 1 of argv) as critical
end run
APPLESCRIPT
}

fail() {
  print -u2 -- "error: $*"
  [[ "${DK2_IMPORT_GUI:-0}" != 1 ]] || show_error "$*"
  exit 1
}

choose_source() {
  /usr/bin/osascript <<'APPLESCRIPT'
POSIX path of (choose folder with prompt "Choose your original Dungeon Keeper 2 folder" default location (path to home folder))
APPLESCRIPT
}

resolve_source() {
  local selected="${1%/}"
  if [[ -f "${selected}/DKII-DX.exe" ]]; then
    print -r -- "${selected}"
    return
  fi

  local matches
  matches=("${(@f)$(/usr/bin/find "${selected}" -maxdepth 4 -type f -iname 'DKII-DX.exe' -print 2>/dev/null)}")
  (( ${#matches} == 1 )) || fail "select the folder that contains DKII-DX.exe"
  print -r -- "${matches[1]:h}"
}

validate_source() {
  local source="$1" exe_hash
  [[ -f "${source}/DKII-DX.exe" ]] || fail "DKII-DX.exe is missing"
  [[ -f "${source}/patch.dll" ]] || fail "the original GOG patch.dll is missing"
  [[ -f "${source}/Data/FrontEnd.WAD" ]] || fail "Data/FrontEnd.WAD is missing"
  [[ -f "${source}/Data/Meshes.WAD" ]] || fail "Data/Meshes.WAD is missing"
  exe_hash="$(/usr/bin/shasum -a 256 "${source}/DKII-DX.exe" | /usr/bin/awk '{print $1}')"
  if [[ "${exe_hash}" != "${KNOWN_GOG_EXE_SHA256}" && "${DK2_ALLOW_UNKNOWN_EXE:-0}" != 1 ]]; then
    fail "unsupported DKII-DX.exe (${exe_hash}); this build currently requires the original GOG 1.7 executable"
  fi
}

find_payload() {
  if [[ -n "${DK2_FLAME_PAYLOAD:-}" ]]; then
    print -r -- "${DK2_FLAME_PAYLOAD}"
    return
  fi
  local candidates
  candidates=("${(@f)$(/usr/bin/find "${REPO_ROOT}/.cache/native-metal-ci" -mindepth 2 -maxdepth 2 -type f -name PATCH.dll -print 2>/dev/null | /usr/bin/sort)}")
  (( ${#candidates} > 0 )) || fail "Flame payload is missing; set DK2_FLAME_PAYLOAD"
  print -r -- "${candidates[-1]:h}"
}

wine_reg_add() {
  env WINEPREFIX="${PREFIX}" WINEDEBUG=-all MVK_CONFIG_LOG_LEVEL=0 \
    "${WINE}" reg add "$1" /v "$2" /t "$3" /d "$4" /f >/dev/null
}

selected="${2:-${1:-}}"
check_only=0
if [[ "${1:-}" == '--check' ]]; then
  check_only=1
elif [[ -z "${selected}" ]]; then
  selected="$(choose_source)" || exit 2
fi

source_game="$(resolve_source "${selected}")"
validate_source "${source_game}"
print -- "Validated: ${source_game}"
(( check_only )) && exit 0

[[ ! -e "${GAME_DIR}" ]] || fail "an imported game already exists at ${GAME_DIR}"
[[ -x "${WINE}" && -x "${WINESERVER}" ]] || fail "the bundled Wine runtime is missing"
payload="$(find_payload)"
[[ -f "${payload}/PATCH.dll" ]] || fail "PATCH.dll is missing from ${payload}"
[[ -f "${payload}/flame/Flame.dll" ]] || fail "Flame.dll is missing from ${payload}"
[[ -f "${payload}/flame/DKII.dll" ]] || fail "DKII.dll is missing from ${payload}"

/bin/mkdir -p "${PREFIX}" "${GAME_PARENT}"
if [[ ! -f "${PREFIX}/system.reg" ]]; then
  env WINEPREFIX="${PREFIX}" WINEARCH=win64 WINEDEBUG=-all \
    MVK_CONFIG_LOG_LEVEL=0 \
    WINEDLLOVERRIDES='mscoree,mshtml=' "${WINE}" wineboot -u >/dev/null
fi
env WINEPREFIX="${PREFIX}" "${WINESERVER}" -k >/dev/null 2>&1 || true
env WINEPREFIX="${PREFIX}" "${WINESERVER}" -w >/dev/null 2>&1 || true

stage="${GAME_PARENT}/.Dungeon Keeper 2.importing.$$"
trap '/bin/rm -rf -- "${stage}"' EXIT INT TERM HUP
[[ ! -e "${stage}" ]] || fail "stale import staging path: ${stage}"
/usr/bin/ditto "${source_game}" "${stage}"
/bin/cp -p "${stage}/patch.dll" "${stage}/patch.gog.dll"
for dll in ddraw.dll d3dimm.dll dinput.dll; do
  [[ ! -f "${stage}/${dll}" ]] || /bin/mv "${stage}/${dll}" "${stage}/${dll}.disabled"
done
/bin/mkdir -p "${stage}/flame"
/bin/cp -p "${payload}/PATCH.dll" "${stage}/PATCH.dll"
/bin/cp -p "${payload}/flame/Flame.dll" "${stage}/Flame.dll"
/bin/cp -p "${payload}/flame/DKII.dll" "${stage}/DKII.dll"
/bin/cp -p "${payload}/flame/Flame.dll" "${stage}/flame/Flame.dll"
/bin/cp -p "${payload}/flame/DKII.dll" "${stage}/flame/DKII.dll"
/bin/mv "${stage}" "${GAME_DIR}"
trap - EXIT INT TERM HUP

wine_reg_add 'HKCU\Software\Wine\Direct3D' VideoMemorySize REG_SZ 2048
wine_reg_add 'HKCU\Software\Wine\DirectInput' MouseWarpOverride REG_SZ disable
wine_reg_add 'HKCU\Software\Bullfrog Productions Ltd\Dungeon Keeper II\Configuration\Video' 'Res 1024*768 Enable' REG_DWORD 1
wine_reg_add 'HKCU\Software\Bullfrog Productions Ltd\Dungeon Keeper II\Configuration\Video' 'Screen Width' REG_DWORD 1024
wine_reg_add 'HKCU\Software\Bullfrog Productions Ltd\Dungeon Keeper II\Configuration\Video' 'Screen Height' REG_DWORD 768
env WINEPREFIX="${PREFIX}" "${WINESERVER}" -k >/dev/null 2>&1 || true

for link in "${PREFIX}/dosdevices/"*(N); do
  [[ "${link:t}" == 'c:' ]] && continue
  [[ -L "${link}" ]] && /bin/unlink "${link}"
done
for user_dir in "${PREFIX}/drive_c/users/"*(N/); do
  for name in Desktop Documents Downloads Music Pictures Videos; do
    user_path="${user_dir}/${name}"
    [[ -L "${user_path}" ]] && /bin/unlink "${user_path}"
    /bin/mkdir -p "${user_path}"
  done
done

print -- "Imported: ${GAME_DIR}"
