#!/bin/zsh
# Deprecated: kept only as a compatibility shim for old muscle-memory
# invocations and launch commands. Use macos/dk2-runner.zsh directly.
set -euo pipefail
exec "${0:A:h}/dk2-runner.zsh" "$@"
