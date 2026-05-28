#!/bin/bash
# Generate logo.icns from logo.png (silently).
set -e
LOGO="$1"
ICONSET="$2"
OUT="$3"

rm -rf "$ICONSET"
mkdir -p "$ICONSET"

for sz in 16 32 128 256 512; do
	sips -z $sz $sz "$LOGO" --out "$ICONSET/icon_${sz}x${sz}.png" >/dev/null 2>&1
	sips -z $((sz * 2)) $((sz * 2)) "$LOGO" --out "$ICONSET/icon_${sz}x${sz}@2x.png" >/dev/null 2>&1
done
sips -z 1024 1024 "$LOGO" --out "$ICONSET/icon_512x512@2x.png" >/dev/null 2>&1

mkdir -p "$(dirname "$OUT")"
iconutil -c icns "$ICONSET" -o "$OUT"
