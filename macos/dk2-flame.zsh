#!/bin/zsh
set -u

readonly CONTENTS="${0:A:h:h}"
readonly RUNTIME="${CONTENTS}/Resources/wine"
readonly PREFIX="${HOME}/Library/Application Support/Dungeon Keeper 2 Flame/prefix-native"
readonly GAME_DIR="${PREFIX}/drive_c/GOG Games/Dungeon Keeper 2"
readonly LOG_DIR="${HOME}/Library/Logs/Dungeon Keeper 2 Flame"
readonly LOG_FILE="${LOG_DIR}/game.log"

show_error() {
  /usr/bin/osascript -e "display alert \"Dungeon Keeper 2 Flame\" message \"$1\" as critical" >/dev/null
}

if [[ ! -x "${RUNTIME}/bin/wine" ]]; then
  show_error "Встроенный Wine повреждён. Пересоберите приложение."
  exit 1
fi

if [[ ! -f "${GAME_DIR}/DKII-DX.exe" || ! -f "${GAME_DIR}/flame/Flame.dll" ]]; then
  show_error "Не найдена изолированная установка игры. Запустите macos/build-wrapper.zsh."
  exit 1
fi

/bin/mkdir -p "${LOG_DIR}"
: >| "${LOG_FILE}"

export WINEPREFIX="${PREFIX}"
export WINEDEBUG="-all"
export WINEDLLOVERRIDES="ddraw,d3dimm,dinput=b;winedbg.exe=d"

cd "${GAME_DIR}" || exit 1
"${RUNTIME}/bin/wine" 'C:\GOG Games\Dungeon Keeper 2\DKII-DX.exe' \
  -skip-launcher \
  -flame:windowed \
  -flame:no-initial-size \
  -flame:game-res=1024x768 \
  -nomovies \
  -disablegamma \
  >>"${LOG_FILE}" 2>&1

exit_code=$?
if (( exit_code != 0 )); then
  show_error "Игра завершилась с ошибкой. Журнал: ${LOG_FILE}"
fi
exit ${exit_code}
