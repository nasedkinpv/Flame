#!/bin/zsh
set -euo pipefail

export MVK_CONFIG_LOG_LEVEL=0

readonly SCRIPT_DIR="${0:A:h}"
readonly REPO_ROOT="${SCRIPT_DIR:h}"
readonly SOURCE_PREFIX="${DK2_SOURCE_PREFIX:-${HOME}/.wine}"
readonly DATA_ROOT="${DK2_FLAME_DATA_DIR:-${HOME}/Library/Application Support/Dungeon Keeper 2 Flame}"
readonly PREFIX="${DATA_ROOT}/prefix-native"
readonly GAME_RELATIVE="drive_c/GOG Games/Dungeon Keeper 2"
readonly CACHE_DIR="${REPO_ROOT}/.cache"
readonly DIST_DIR="${REPO_ROOT}/dist"
readonly APP_PATH="${DIST_DIR}/Dungeon Keeper 2 Flame.app"

readonly FLAME_ARCHIVE="${CACHE_DIR}/Flame-1.7.0-260718-macos-native.zip"
readonly FLAME_URL="https://github.com/nasedkinpv/Flame/releases/download/v1.7.0-260718-macos-native/Flame-1.7.0-260718-macos-native.zip"
readonly FLAME_SHA256="a529723f0ce6b5fbc4375170739218d2bbe79d1a8f92ef07c8d1e120053533f9"

readonly WINE_ARCHIVE="${CACHE_DIR}/wine-stable-11.0_1-osx64.tar.xz"
readonly WINE_URL="https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.0_1/wine-stable-11.0_1-osx64.tar.xz"
readonly WINE_SHA256="b50dc50ec7f41d58b115a6b685d4d1315ba3c797bd3aa0f49213f2703cb82388"
readonly WINE_CACHE_ROOT="${CACHE_DIR}/wine-stable-11.0_1"
readonly WINE_CACHE="${WINE_CACHE_ROOT}/Contents/Resources/wine"

fail() {
  print -u2 -- "error: $*"
  exit 1
}

sha256() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

download_verified() {
  local url="$1" output="$2" expected="$3"
  /bin/mkdir -p "${output:h}"
  if [[ ! -f "${output}" || "$(sha256 "${output}")" != "${expected}" ]]; then
    /usr/bin/curl --fail --location --retry 3 --progress-bar "${url}" --output "${output}"
  fi
  [[ "$(sha256 "${output}")" == "${expected}" ]] || fail "checksum mismatch: ${output:t}"
}

wine_reg_add() {
  local prefix="$1" wine="$2"
  (
    cd "${prefix}/drive_c" || exit 1
    env WINEPREFIX="${prefix}" WINEDEBUG=-all "${wine}" reg add "$3" /v "$4" /t "$5" /d "$6" /f >/dev/null
  )
}

wine_reg_delete() {
  local prefix="$1" wine="$2"
  (
    cd "${prefix}/drive_c" || exit 1
    env WINEPREFIX="${prefix}" WINEDEBUG=-all "${wine}" reg delete "$3" /v "$4" /f >/dev/null 2>&1 || true
  )
}

set_flame_config_value() {
  local config="$1" key="$2" value="$3"
  [[ -f "${config}" ]] || return 0
  if /usr/bin/grep -q "^${key}[[:space:]]*=" "${config}"; then
    /usr/bin/sed -i '' -E "s|^${key}[[:space:]]*=.*$|${key} = ${value}|" "${config}"
  fi
}

disable_local_compatibility_dlls() {
  local game_dir="$1" file disabled
  while IFS= read -r -d '' file; do
    disabled="${file}.disabled"
    if [[ -e "${disabled}" ]]; then
      /bin/rm -f -- "${file}"
    else
      /bin/mv -- "${file}" "${disabled}"
    fi
  done < <(/usr/bin/find "${game_dir}" -maxdepth 1 -type f \( -iname ddraw.dll -o -iname d3dimm.dll -o -iname dinput.dll \) -print0)
}

source_game="${SOURCE_PREFIX}/${GAME_RELATIVE}"
[[ -f "${source_game}/DKII-DX.exe" ]] || fail "DKII-DX.exe not found in ${SOURCE_PREFIX}"
/bin/mkdir -p "${CACHE_DIR}" "${DIST_DIR}" "${DATA_ROOT}"

download_verified "${FLAME_URL}" "${FLAME_ARCHIVE}" "${FLAME_SHA256}"
download_verified "${WINE_URL}" "${WINE_ARCHIVE}" "${WINE_SHA256}"

if [[ ! -x "${WINE_CACHE}/bin/wine" ]]; then
  [[ ! -e "${WINE_CACHE_ROOT}" ]] || fail "incomplete Wine cache: ${WINE_CACHE_ROOT}"
  /bin/mkdir -p "${WINE_CACHE_ROOT}"
  /usr/bin/tar -xJf "${WINE_ARCHIVE}" -C "${WINE_CACHE_ROOT}" --strip-components=1
fi
[[ "$("${WINE_CACHE}/bin/wine" --version)" == "wine-11.0" ]] || fail "unexpected Wine version"

app_stage_root="$(/usr/bin/mktemp -d "${DIST_DIR}/.app-build.XXXXXX")"
trap '/bin/rm -rf -- "${app_stage_root}"' EXIT
app_stage="${app_stage_root}/Dungeon Keeper 2 Flame.app"
/bin/mkdir -p "${app_stage}/Contents/MacOS" "${app_stage}/Contents/Resources"
/bin/cp "${SCRIPT_DIR}/Info.plist" "${app_stage}/Contents/Info.plist"
/bin/cp "${SCRIPT_DIR}/dk2-flame.zsh" "${app_stage}/Contents/MacOS/dk2-flame"
/bin/chmod 755 "${app_stage}/Contents/MacOS/dk2-flame"
/usr/bin/ditto "${WINE_CACHE}" "${app_stage}/Contents/Resources/wine"

icon_source="${source_game}/goggame-1207658959.ico"
if [[ -f "${icon_source}" ]]; then
  iconset="${app_stage_root}/AppIcon.iconset"
  /bin/mkdir -p "${iconset}"
  /usr/bin/sips -s format png "${icon_source}" --out "${app_stage_root}/icon.png" >/dev/null
  for size in 16 32 128 256 512; do
    /usr/bin/sips -z ${size} ${size} "${app_stage_root}/icon.png" --out "${iconset}/icon_${size}x${size}.png" >/dev/null
    double=$(( size * 2 ))
    /usr/bin/sips -z ${double} ${double} "${app_stage_root}/icon.png" --out "${iconset}/icon_${size}x${size}@2x.png" >/dev/null
  done
  /usr/bin/iconutil -c icns "${iconset}" -o "${app_stage}/Contents/Resources/AppIcon.icns"
fi

/usr/bin/xattr -dr com.apple.quarantine "${app_stage}" 2>/dev/null || true
/usr/bin/plutil -lint "${app_stage}/Contents/Info.plist" >/dev/null
/usr/bin/codesign --force --deep --sign - --timestamp=none "${app_stage}" >/dev/null
/usr/bin/codesign --verify --deep --strict "${app_stage}"

[[ "${APP_PATH}" == "${REPO_ROOT}/dist/Dungeon Keeper 2 Flame.app" ]] || fail "unexpected app path"
[[ ! -e "${APP_PATH}" ]] || /bin/rm -rf -- "${APP_PATH}"
/bin/mv "${app_stage}" "${APP_PATH}"
/bin/rm -rf -- "${app_stage_root}"
trap - EXIT

wine_bin="${APP_PATH}/Contents/Resources/wine/bin/wine"
wineserver_bin="${APP_PATH}/Contents/Resources/wine/bin/wineserver"

if [[ ! -d "${PREFIX}" ]]; then
  prefix_stage_root="$(/usr/bin/mktemp -d "${DATA_ROOT}/.prefix-build.XXXXXX")"
  trap '/bin/rm -rf -- "${prefix_stage_root}"' EXIT
  prefix_stage="${prefix_stage_root}/prefix-native"
  env WINEPREFIX="${prefix_stage}" WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=' "${wine_bin}" wineboot -u >/dev/null
  /bin/mkdir -p "${prefix_stage}/drive_c/GOG Games"
  /usr/bin/ditto "${source_game}" "${prefix_stage}/drive_c/GOG Games/Dungeon Keeper 2"
  /bin/mv "${prefix_stage}" "${PREFIX}"
  /bin/rm -rf -- "${prefix_stage_root}"
  trap - EXIT
fi

env WINEPREFIX="${PREFIX}" "${wineserver_bin}" -k 2>/dev/null || true
game_dir="${PREFIX}/${GAME_RELATIVE}"
[[ -f "${game_dir}/patch.dll" ]] || fail "GOG patch.dll not found"
[[ -f "${game_dir}/patch.gog.dll" ]] || /bin/cp -p "${game_dir}/patch.dll" "${game_dir}/patch.gog.dll"
disable_local_compatibility_dlls "${game_dir}"

flame_stage="$(/usr/bin/mktemp -d "${DATA_ROOT}/.flame-update.XXXXXX")"
trap '/bin/rm -rf -- "${flame_stage}"' EXIT
/usr/bin/unzip -oq "${FLAME_ARCHIVE}" -d "${flame_stage}"
/usr/bin/ditto "${flame_stage}" "${game_dir}"
/bin/rm -rf -- "${flame_stage}"
trap - EXIT

[[ -f "${game_dir}/flame/Flame.dll" ]] || fail "Flame.dll is missing"
[[ -f "${game_dir}/flame/DKII.dll" ]] || fail "DKII.dll is missing"
/bin/cp -p "${game_dir}/flame/Flame.dll" "${game_dir}/Flame.dll"
/bin/cp -p "${game_dir}/flame/DKII.dll" "${game_dir}/DKII.dll"

wine_reg_add "${PREFIX}" "${wine_bin}" 'HKCU\Software\Wine\Direct3D' VideoMemorySize REG_SZ 2048
wine_reg_delete "${PREFIX}" "${wine_bin}" 'HKCU\Software\Wine\Direct3D' renderer
wine_reg_delete "${PREFIX}" "${wine_bin}" 'HKCU\Software\Wine\Direct3D' csmt
wine_reg_add "${PREFIX}" "${wine_bin}" 'HKCU\Software\Wine\DirectInput' MouseWarpOverride REG_SZ disable
wine_reg_delete "${PREFIX}" "${wine_bin}" 'HKCU\Software\Wine\Mac Driver' RetinaMode
wine_reg_add "${PREFIX}" "${wine_bin}" 'HKCU\Software\Bullfrog Productions Ltd\Dungeon Keeper II\Configuration\Video' 'Res 1024*768 Enable' REG_DWORD 1
wine_reg_add "${PREFIX}" "${wine_bin}" 'HKCU\Software\Bullfrog Productions Ltd\Dungeon Keeper II\Configuration\Video' 'Screen Width' REG_DWORD 640
wine_reg_add "${PREFIX}" "${wine_bin}" 'HKCU\Software\Bullfrog Productions Ltd\Dungeon Keeper II\Configuration\Video' 'Screen Height' REG_DWORD 480
wine_reg_delete "${PREFIX}" "${wine_bin}" 'HKCU\Software\Wine\Explorer' Desktop
wine_reg_delete "${PREFIX}" "${wine_bin}" 'HKCU\Software\Wine\Explorer\Desktops' Default

config="${game_dir}/flame/config.toml"
set_flame_config_value "${config}" Screen_Width 640
set_flame_config_value "${config}" Screen_Height 480
set_flame_config_value "${config}" Screen_Windowed false
set_flame_config_value "${config}" game-res '""'
set_flame_config_value "${config}" no-initial-size true
set_flame_config_value "${config}" lock-window-size false
set_flame_config_value "${config}" single-core true
env WINEPREFIX="${PREFIX}" "${wineserver_bin}" -k 2>/dev/null || true

for link in "${PREFIX}/dosdevices/"*(N); do
  [[ "${link:t}" == "c:" ]] && continue
  [[ -L "${link}" ]] && /bin/unlink "${link}"
done
for user_dir in "${PREFIX}/drive_c/users/"*(N/); do
  for name in Desktop Documents Downloads Music Pictures Videos; do
    user_path="${user_dir}/${name}"
    [[ -L "${user_path}" ]] && /bin/unlink "${user_path}"
    /bin/mkdir -p "${user_path}"
  done
done

[[ ! -e "${PREFIX}/dosdevices/z:" ]] || fail "Wine Z: drive must not be present"
/usr/bin/codesign --verify --deep --strict "${APP_PATH}"

print -- "App: ${APP_PATH}"
print -- "Prefix: ${PREFIX}"
