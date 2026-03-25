#!/bin/bash
set -e

OUT_DIR="firmware-bin"
mkdir -p "$OUT_DIR"

echo "Generating build info..."
echo "#define BUILD_TIME \"$(date '+%Y-%m-%d %H:%M:%S')\"" > include/build_info.h

echo "Building firmware..."
pio run

TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
FILENAME="firmware_${TIMESTAMP}.bin"

cp .pio/build/esp32/firmware.bin "$OUT_DIR/$FILENAME"

SIZE=$(stat -f%z "$OUT_DIR/$FILENAME" 2>/dev/null || stat -c%s "$OUT_DIR/$FILENAME")
echo "Done! firmware/$FILENAME ($(( SIZE / 1024 )) KB)"
