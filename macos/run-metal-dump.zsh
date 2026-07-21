#!/bin/zsh
# Named texture-dump session: enables the game-side flametal:TextureDump
# option (decoded surfaces written once as PNG with their REAL resource
# names, before atlas compositing) for one run, restoring the config after.
# Play a level to capture terrain/creature/spell textures; the menu alone
# yields only menu resources. Output: prefix/drive_c/dk2-texdump ->
#   ~/Library/Application Support/Dungeon Keeper II/prefix/drive_c/dk2-texdump
#
# The legacy host-side page dump (hash-named composited pages) is still
# available via DK2_TEXTURE_DUMP with run-metal-game.zsh if ever needed.
set -euo pipefail

readonly SCRIPT_DIR="${0:A:h}"
readonly APP="${DK2_METAL_APP_BUNDLE:-${SCRIPT_DIR}/native/build/Dungeon Keeper II.app}"
readonly RUNNER="${SCRIPT_DIR}/dk2-runner.zsh"
readonly PREFIX="${DK2_METAL_PREFIX:-${HOME}/Library/Application Support/Dungeon Keeper II/prefix}"
readonly BRIDGE_FILE="${PREFIX}/drive_c/dk2-metal/frame.bin"
readonly SHADOW_LEVEL="${DK2_SHADOW_LEVEL:-3}"
readonly CONFIG="${PREFIX}/drive_c/GOG Games/Dungeon Keeper 2/flametal/config.toml"
readonly DUMP_DIR="${PREFIX}/drive_c/dk2-texdump"

if (( $# == 0 )); then
  LEVEL=''
elif (( $# == 2 )) && [[ "$1" == '-LEVEL' ]]; then
  LEVEL="$2"
else
  print -u2 -- "usage: ${0:t} [-LEVEL levelN]"
  exit 2
fi

[[ -f "${CONFIG}" ]] || { print -u2 -- "no flametal config at ${CONFIG}"; exit 1 }

# Enable the option for this session only. The game rewrites config.toml on
# exit with every registered option present, so edit the existing key.
restore_config() {
  /usr/bin/sed -i '' 's|^TextureDump = .*|TextureDump = ""|' "${CONFIG}" || true
}
trap restore_config EXIT INT TERM
if grep -q '^TextureDump = ' "${CONFIG}"; then
  /usr/bin/sed -i '' 's|^TextureDump = .*|TextureDump = "C:\\\\dk2-texdump"|' "${CONFIG}"
else
  /usr/bin/sed -i '' 's|^\[flametal\]|[flametal]\nTextureDump = "C:\\\\dk2-texdump"|' "${CONFIG}"
fi

print -- "named texture dump -> ${DUMP_DIR}"
print -- "play through the content you want captured, then quit the game"

DK2_LEVEL="${LEVEL}" DK2_SHADOW_LEVEL="${SHADOW_LEVEL}" \
  "${APP}/Contents/MacOS/DK2Metal" \
    --fullscreen \
    --bridge-file="${BRIDGE_FILE}" \
    --game-runner="${RUNNER}"

print -- "dump session finished:"
/bin/ls "${DUMP_DIR}" 2>/dev/null | /usr/bin/wc -l | /usr/bin/tr -d ' ' | /usr/bin/sed 's/$/ files/'
print -- "${DUMP_DIR}"
