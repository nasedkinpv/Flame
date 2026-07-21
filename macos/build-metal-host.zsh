#!/bin/zsh
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly REPO_ROOT="${SCRIPT_DIR:h}"
readonly SOURCE="${SCRIPT_DIR}/native/DK2Metal.mm"
readonly BUILD_ROOT="${SCRIPT_DIR}/native/build"
readonly APP="${BUILD_ROOT}/Dungeon Keeper II.app"
readonly EXECUTABLE="${APP}/Contents/MacOS/DK2Metal"
readonly PRODUCER="${BUILD_ROOT}/DK2BridgeProducer"
readonly SIGNING_IDENTITY="${DK2_CODESIGN_IDENTITY:--}"

[[ "$(/usr/bin/uname -m)" == "arm64" ]] || { print -u2 -- "error: ARM64 host required"; exit 1; }
[[ "$(/usr/bin/xcrun --sdk macosx --show-sdk-version)" == 26.* ]] || {
  print -u2 -- "error: Xcode 26 SDK required"
  exit 1
}

/bin/rm -rf -- "${APP}"
/bin/mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources"
/bin/cp "${SCRIPT_DIR}/native/Info.plist" "${APP}/Contents/Info.plist"
/bin/cp "${SCRIPT_DIR}/AppIcon.icns" "${APP}/Contents/Resources/AppIcon.icns"

/usr/bin/xcrun --sdk macosx metal \
  -c "${SCRIPT_DIR}/native/DK2Shaders.metal" \
  -o "${BUILD_ROOT}/DK2Shaders.air"
/usr/bin/xcrun --sdk macosx metallib \
  "${BUILD_ROOT}/DK2Shaders.air" \
  -o "${APP}/Contents/Resources/DK2Shaders.metallib"

/usr/bin/xcrun --sdk macosx clang++ \
  -std=c++20 \
  -O3 \
  -fobjc-arc \
  -arch arm64 \
  -mmacosx-version-min=26.0 \
  -Wall -Wextra -Werror \
  -framework AppKit \
  -framework GameController \
  -framework ImageIO \
  -framework Metal \
  -framework QuartzCore \
  -I "${REPO_ROOT}/src/gog_patch_dll" \
  "${SOURCE}" \
  -o "${EXECUTABLE}"

/usr/bin/xcrun --sdk macosx clang++ \
  -std=c++20 \
  -O3 \
  -arch arm64 \
  -mmacosx-version-min=26.0 \
  -Wall -Wextra -Werror \
  -I "${REPO_ROOT}/src/gog_patch_dll" \
  "${SCRIPT_DIR}/native/BridgeProducer.cpp" \
  -o "${PRODUCER}"

/usr/bin/plutil -lint "${APP}/Contents/Info.plist" >/dev/null
if [[ "${SIGNING_IDENTITY}" == "-" ]]; then
  /usr/bin/codesign --force --sign - --timestamp=none "${APP}" >/dev/null
else
  /usr/bin/codesign --force --sign "${SIGNING_IDENTITY}" --timestamp \
    --options runtime \
    --entitlements "${SCRIPT_DIR}/native/DK2Metal.entitlements" \
    "${APP}" >/dev/null
fi
/usr/bin/codesign --verify --deep --strict "${APP}"
/usr/bin/file "${EXECUTABLE}"
print -- "${APP}"
