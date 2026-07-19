#!/bin/zsh
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly REPO_ROOT="${SCRIPT_DIR:h}"
readonly CACHE_DIR="${REPO_ROOT}/.cache"
readonly DIST_DIR="${REPO_ROOT}/dist"
readonly APP_PATH="${DIST_DIR}/Dungeon Keeper II.app"
readonly NATIVE_APP="${SCRIPT_DIR}/native/build/Dungeon Keeper II.app"
readonly WINE_ARCHIVE="${CACHE_DIR}/wine-stable-11.0_1-osx64.tar.xz"
readonly WINE_URL='https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.0_1/wine-stable-11.0_1-osx64.tar.xz'
readonly WINE_SHA256='b50dc50ec7f41d58b115a6b685d4d1315ba3c797bd3aa0f49213f2703cb82388'
readonly WINE_CACHE_ROOT="${CACHE_DIR}/wine-stable-11.0_1"
readonly WINE_CACHE="${WINE_CACHE_ROOT}/Contents/Resources/wine"
readonly SIGNING_IDENTITY="${DK2_CODESIGN_IDENTITY:--}"

fail() {
  print -u2 -- "error: $*"
  exit 1
}

sha256() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

find_payload() {
  if [[ -n "${DK2_FLAME_PAYLOAD:-}" ]]; then
    print -r -- "${DK2_FLAME_PAYLOAD}"
    return
  fi
  local candidates
  candidates=("${(@f)$(/usr/bin/find "${CACHE_DIR}/native-metal-ci" -mindepth 2 -maxdepth 2 -type f -name PATCH.dll -print 2>/dev/null | /usr/bin/sort)}")
  (( ${#candidates} > 0 )) || fail "set DK2_FLAME_PAYLOAD to a Release artifact directory"
  print -r -- "${candidates[-1]:h}"
}

"${SCRIPT_DIR}/build-metal-host.zsh" >/dev/null
if [[ ! -f "${WINE_ARCHIVE}" || "$(sha256 "${WINE_ARCHIVE}")" != "${WINE_SHA256}" ]]; then
  /bin/mkdir -p "${CACHE_DIR}"
  /usr/bin/curl --fail --location --retry 3 --progress-bar "${WINE_URL}" --output "${WINE_ARCHIVE}"
fi
[[ "$(sha256 "${WINE_ARCHIVE}")" == "${WINE_SHA256}" ]] || fail "Wine archive checksum mismatch"
if [[ ! -x "${WINE_CACHE}/bin/wine" ]]; then
  [[ ! -e "${WINE_CACHE_ROOT}" ]] || fail "incomplete Wine cache: ${WINE_CACHE_ROOT}"
  /bin/mkdir -p "${WINE_CACHE_ROOT}"
  /usr/bin/tar -xJf "${WINE_ARCHIVE}" -C "${WINE_CACHE_ROOT}" --strip-components=1
fi
payload="$(find_payload)"
[[ -f "${payload}/PATCH.dll" ]] || fail "PATCH.dll is missing from ${payload}"
[[ -f "${payload}/flame/Flame.dll" ]] || fail "Flame.dll is missing from ${payload}"
[[ -f "${payload}/flame/DKII.dll" ]] || fail "DKII.dll is missing from ${payload}"

/bin/mkdir -p "${DIST_DIR}"
stage_root="$(/usr/bin/mktemp -d "${DIST_DIR}/.metal-app-build.XXXXXX")"
trap '/bin/rm -rf -- "${stage_root}"' EXIT
stage="${stage_root}/Dungeon Keeper II.app"
/bin/mkdir -p "${stage}/Contents/MacOS" "${stage}/Contents/Resources/Flame/flame"
/bin/cp "${SCRIPT_DIR}/MetalInfo.plist" "${stage}/Contents/Info.plist"
/bin/cp "${SCRIPT_DIR}/dk2-metal-launcher.zsh" "${stage}/Contents/MacOS/dk2-metal-launcher"
/bin/cp "${NATIVE_APP}/Contents/MacOS/DK2Metal" "${stage}/Contents/MacOS/DK2Metal"
/bin/cp "${NATIVE_APP}/Contents/Resources/DK2Shaders.metallib" "${stage}/Contents/Resources/DK2Shaders.metallib"
/bin/cp "${SCRIPT_DIR}/import-original-game.zsh" "${stage}/Contents/Resources/import-original-game"
/bin/cp "${payload}/PATCH.dll" "${stage}/Contents/Resources/Flame/PATCH.dll"
/bin/cp "${payload}/flame/Flame.dll" "${stage}/Contents/Resources/Flame/flame/Flame.dll"
/bin/cp "${payload}/flame/DKII.dll" "${stage}/Contents/Resources/Flame/flame/DKII.dll"
/usr/bin/ditto "${WINE_CACHE}" "${stage}/Contents/Resources/wine"
/bin/chmod 755 "${stage}/Contents/MacOS/dk2-metal-launcher" \
  "${stage}/Contents/MacOS/DK2Metal" "${stage}/Contents/Resources/import-original-game"
/usr/bin/plutil -lint "${stage}/Contents/Info.plist" >/dev/null
if [[ "${SIGNING_IDENTITY}" == '-' ]]; then
  /usr/bin/codesign --force --deep --sign - --timestamp=none "${stage}" >/dev/null
else
  /usr/bin/codesign --force --deep --sign "${SIGNING_IDENTITY}" --timestamp \
    --options runtime --entitlements "${SCRIPT_DIR}/native/DK2Metal.entitlements" "${stage}" >/dev/null
fi
/usr/bin/codesign --verify --deep --strict "${stage}"

[[ "${APP_PATH}" == "${REPO_ROOT}/dist/Dungeon Keeper II.app" ]] || fail "unexpected app path"
[[ ! -e "${APP_PATH}" ]] || /bin/rm -rf -- "${APP_PATH}"
/bin/mv "${stage}" "${APP_PATH}"
/bin/rm -rf -- "${stage_root}"
trap - EXIT
/usr/bin/codesign --verify --deep --strict "${APP_PATH}"
print -- "${APP_PATH}"
