#!/bin/bash
# Run the real autorun.brs logic in a BrightScript interpreter (npx brs) with stand-ins for the
# BrightSign-only objects, then feed it the ESP32 commands. Also runs the BrighterScript syntax check.
# brs lacks UpTime() (stubbed in harness.brs) and String.Tokenize (swapped for Split in the test copy only).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); CARD="$HERE/.."
W=$(mktemp -d); mkdir -p "$W/source"
cp "$HERE/harness.brs" "$W/source/"
cp "${LIBRARY:-/mnt/media/Pumpkin folder 2026/SD Card 2026/library.json}" "$W/library.json"
cp "$CARD/show.json" "$W/show.json"
sed -e 's/^Sub Main()/Sub RealMain()/' -e 's#"SD:/show_trace.txt"#"tmp:/show_trace.txt"#' -e 's#"SD:/#"pkg:/#g' \
    -e 's/parts = line.Tokenize(" ")/parts = line.Split(" ")/' "$CARD/autorun.brs" > "$W/source/autorun.brs"
echo "== BrighterScript syntax check (BS1129 unknown-component = expected false alarms)"
mkdir -p "$W/bsc/source" && cp "$CARD/autorun.brs" "$W/bsc/source/" && printf 'title=t\n' > "$W/bsc/manifest"
(cd "$W/bsc" && npx -y -p brighterscript bsc --rootDir . --createPackage false 2>&1 | grep " - error" | grep -v BS1129) && { echo "!! REAL SYNTAX ERRORS ABOVE"; exit 1; } || echo "clean"
echo "== Command run"
(cd "$W" && npx -y brs source/harness.brs source/autorun.brs 2>&1 | grep -v "^CARD> SONG\|npm WARN")
rm -rf "$W"
