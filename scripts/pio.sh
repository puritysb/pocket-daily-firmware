#!/bin/bash
set -euo pipefail

if [[ -n "${PLATFORMIO_BIN:-}" ]]; then
  exec "${PLATFORMIO_BIN}" "$@"
fi

APPLE_SILICON=0
if [[ "$(uname -s)" == "Darwin" ]]; then
  if [[ "$(uname -m)" == "arm64" ]] ||
    [[ "$(/usr/sbin/sysctl -n hw.optional.arm64 2>/dev/null || true)" == "1" ]]; then
    APPLE_SILICON=1
  fi
fi

if [[ "$APPLE_SILICON" == "1" && -x /opt/homebrew/bin/pio ]] &&
   [[ "$(/opt/homebrew/bin/pio --version)" == *"version 6.2.0" ]]; then
  # PlatformIO discovers its own executable through PATH for child commands.
  # Keep the arm64 Homebrew prefix ahead of a legacy /usr/local installation.
  PATH="/opt/homebrew/bin:${PATH}" exec /opt/homebrew/bin/pio "$@"
fi

# The SDK's native environment may be newer than Homebrew. Do not pair Core
# 6.1's SCons with the platform's pinned 4.11 package on Apple Silicon.
SDK_PIO="${HOME}/.platformio/penv/bin/pio"
SDK_PYTHON="${HOME}/.platformio/penv/bin/python"
if [[ "$APPLE_SILICON" == "1" && -x "$SDK_PIO" && -x "$SDK_PYTHON" ]] &&
   [[ "$("$SDK_PYTHON" -c 'import platform; print(platform.machine())')" == arm64 ]] &&
   [[ "$("$SDK_PIO" --version)" == *"version 6.2.0" ]]; then
  PATH="$(dirname "$SDK_PIO"):${PATH}" exec "$SDK_PIO" "$@"
fi

if command -v pio >/dev/null 2>&1; then
  exec "$(command -v pio)" "$@"
fi

echo "PlatformIO was not found. Install PlatformIO Core or set PLATFORMIO_BIN." >&2
exit 127
