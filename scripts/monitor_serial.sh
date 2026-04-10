#!/bin/zsh

set -euo pipefail

KILL_EXISTING=0

if [[ "${1:-}" == "--kill-existing" ]]; then
  KILL_EXISTING=1
  shift
fi

PORT="${1:-/dev/cu.usbmodem14341301}"
BAUD="${2:-115200}"

export IDF_PATH=/Users/mark/.espressif/v6.0/esp-idf
export IDF_PYTHON_ENV_PATH=/Users/mark/.espressif/tools/python/v6.0/venv
export ESP_IDF_VERSION=6.0.0
export PATH=/Users/mark/.espressif/tools/python/v6.0/venv/bin:/Users/mark/.espressif/tools/cmake/4.0.3/CMake.app/Contents/bin:/Users/mark/.espressif/tools/ninja/1.12.1:/Users/mark/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$PATH

LOCK_INFO="$(lsof "$PORT" 2>/dev/null || true)"
if [[ -n "$LOCK_INFO" ]]; then
  if [[ "$KILL_EXISTING" == "1" ]]; then
    LOCK_PIDS="$(lsof -t "$PORT" 2>/dev/null | tr '\n' ' ')"
    if [[ -n "$LOCK_PIDS" ]]; then
      echo "Killing existing port owner(s): $LOCK_PIDS"
      kill $LOCK_PIDS
      sleep 1
    fi
  else
    echo "Serial port is already in use: $PORT" >&2
    echo "$LOCK_INFO" >&2
    echo "Re-run with --kill-existing to terminate the current owner first." >&2
    exit 1
  fi
fi

exec python "$IDF_PATH/tools/idf_monitor.py" \
  --port "$PORT" \
  --baud "$BAUD" \
  /Users/mark/Repos/esp32/px-wifi-v1/build/px-wifi-v1.elf