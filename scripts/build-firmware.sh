#!/usr/bin/env bash
#
# Builds the firmware and packages it as a single flashable image plus an
# esp-web-tools manifest, into web/static/firmware/.
#
# Usage: ./scripts/build-firmware.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/web/static/firmware"
ENV_NAME=m5stack-fire

PIO="${PIO:-$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")}"
PYTHON="${PYTHON:-$HOME/.platformio/penv/bin/python}"
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"

[ -f "$ESPTOOL" ] || { echo "missing: $ESPTOOL" >&2; exit 1; }

VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)"

mkdir -p "$OUT"

echo "==> building $ENV_NAME"
"$PIO" run -e "$ENV_NAME" -d "$ROOT"

BUILD="$ROOT/.pio/build/$ENV_NAME"

# Single image starting at 0x0 so the installer writes it in one part.
# No boot_app0: partitions.csv declares a factory app with no OTA data.
"$PYTHON" "$ESPTOOL" --chip esp32 merge_bin \
  -o "$OUT/lyrebird.bin" \
  --flash_mode keep --flash_freq keep --flash_size keep \
  0x1000  "$BUILD/bootloader.bin" \
  0x8000  "$BUILD/partitions.bin" \
  0x10000 "$BUILD/firmware.bin"

cat > "$OUT/manifest.json" <<EOF
{
  "name": "Lyrebird — M5Stack Fire",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [{ "path": "lyrebird.bin", "offset": 0 }]
    }
  ]
}
EOF

# Consumed by the installer (web/src/lib/installer.ts via the page).
cat > "$OUT/firmware.json" <<EOF
{
  "version": "$VERSION",
  "image": "lyrebird.bin",
  "imageOffset": 0
}
EOF

echo "    $OUT/lyrebird.bin ($(du -h "$OUT/lyrebird.bin" | cut -f1))"
echo "done — version $VERSION"
