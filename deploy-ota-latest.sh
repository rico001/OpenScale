#!/bin/bash
set -e

HOST="${1:-openscale.local}"
FILE=$(ls -t firmware-bin/*.bin 2>/dev/null | head -1)

if [ -z "$FILE" ]; then
  echo "Keine .bin Datei in firmware-bin/ gefunden. Erst ./build.sh ausführen."
  exit 1
fi

SIZE=$(stat -f%z "$FILE" 2>/dev/null || stat -c%s "$FILE")
echo "Uploading: $FILE ($(( SIZE / 1024 )) KB) → http://$HOST/ota"

curl -f --progress-bar -F "firmware=@$FILE" "http://$HOST/ota" && echo "Upload erfolgreich! ESP32 startet neu..." || echo "Upload fehlgeschlagen."
