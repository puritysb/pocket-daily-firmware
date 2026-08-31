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

if [[ "$APPLE_SILICON" == "1" && -x /opt/homebrew/bin/pio ]]; then
  # PlatformIO discovers its own executable through PATH for child commands.
  # Keep the arm64 Homebrew prefix ahead of a legacy /usr/local installation.
  PATH="/opt/homebrew/bin:${PATH}" exec /opt/homebrew/bin/pio "$@"
fi

if command -v pio >/dev/null 2>&1; then
  exec "$(command -v pio)" "$@"
fi

echo "PlatformIO was not found. Install PlatformIO Core or set PLATFORMIO_BIN." >&2
exit 127
