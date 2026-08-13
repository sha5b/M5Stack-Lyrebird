#!/usr/bin/env bash
#
# Builds every board's firmware and packages each as a single flashable image
# plus a manifest, into web/static/firmware/.
#
# The Fire is an ESP32 and the CoreS3 an ESP32-S3 — different architectures, so
# there are two binaries and the website has to ask which board you have. Note
# the bootloader offset differs with the chip: 0x1000 on the ESP32, 0x0 on the
# ESP32-S3. Getting that wrong produces an image that flashes cleanly and then
# will not boot.
#
# Usage: ./scripts/build-firmware.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/web/static/firmware"

PIO="${PIO:-$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")}"
PYTHON="${PYTHON:-$HOME/.platformio/penv/bin/python}"
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"

[ -f "$ESPTOOL" ] || { echo "missing: $ESPTOOL" >&2; exit 1; }

VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)"

mkdir -p "$OUT"

# id | pio env | esptool chip | bootloader offset | chipFamily | display name
BOARDS=(
  "fire|m5stack-fire|esp32|0x1000|ESP32|M5Stack Fire"
  "cores3|m5stack-cores3|esp32s3|0x0|ESP32-S3|M5Stack CoreS3"
)

ENTRIES=()

for spec in "${BOARDS[@]}"; do
  IFS='|' read -r ID ENV_NAME CHIP BL_OFFSET FAMILY NAME <<< "$spec"
  IMAGE="lyrebird-$ID.bin"
  BUILD="$ROOT/.pio/build/$ENV_NAME"

  echo "==> building $ENV_NAME ($CHIP)"
  "$PIO" run -e "$ENV_NAME" -d "$ROOT"

  # One image starting at 0x0 so the installer writes it in a single part.
  # No boot_app0: partitions.csv declares a factory app with no OTA data.
  "$PYTHON" "$ESPTOOL" --chip "$CHIP" merge_bin \
    -o "$OUT/$IMAGE" \
    --flash_mode keep --flash_freq keep --flash_size keep \
    "$BL_OFFSET" "$BUILD/bootloader.bin" \
    0x8000      "$BUILD/partitions.bin" \
    0x10000     "$BUILD/firmware.bin"

  cat > "$OUT/manifest-$ID.json" <<EOF
{
  "name": "Lyrebird — $NAME",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "$FAMILY",
      "parts": [{ "path": "$IMAGE", "offset": 0 }]
    }
  ]
}
EOF

  ENTRIES+=("    {
      \"id\": \"$ID\",
      \"name\": \"$NAME\",
      \"chip\": \"$CHIP\",
      \"chipFamily\": \"$FAMILY\",
      \"image\": \"$IMAGE\",
      \"imageOffset\": 0
    }")

  echo "    $OUT/$IMAGE ($(du -h "$OUT/$IMAGE" | cut -f1))"
done

# Consumed by the installer (web/src/lib/installer.ts via the page).
{
  echo "{"
  echo "  \"version\": \"$VERSION\","
  echo "  \"boards\": ["
  printf '%s' "$(IFS=,; echo "${ENTRIES[*]}")"
  echo ""
  echo "  ]"
  echo "}"
} > "$OUT/firmware.json"

echo "done — version $VERSION"
