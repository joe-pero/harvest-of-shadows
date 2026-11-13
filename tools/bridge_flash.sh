#!/usr/bin/env bash
# Flash an ESP32 that is plugged into a Raspberry Pi "USB bridge", safely.
# Guide: docs/remote-flashing-over-a-pi.md
#
#   BRIDGE=pi@yard-bridge.local bash tools/bridge_flash.sh firmware.bin [0x10000] [/dev/ttyUSB0]
#
# What it checks, in order (it stops at the first problem):
#   1. the local firmware file exists and isn't empty or all zeros
#   2. copies it to the Pi, runs `sync` there, and compares checksums (a power cut right after a
#      copy once left a zero-filled file that esptool happily flashed)
#   3. nothing on the Pi has the serial port open (a running monitor makes the flash fail or hang)
#   4. flashes with --no-stub (works with Debian's packaged esptool) and reads the chip id first
set -euo pipefail

BIN=${1:?usage: BRIDGE=user@host bash tools/bridge_flash.sh firmware.bin [offset] [port]}
OFFSET=${2:-0x10000}
PORT=${3:-/dev/ttyUSB0}
BRIDGE=${BRIDGE:?set BRIDGE=user@host for the Pi}
REMOTE=/tmp/bridge_flash_$$.bin

[ -s "$BIN" ] || { echo "error: $BIN is missing or empty" >&2; exit 1; }
if ! tr -d '\000' < "$BIN" | head -c 1 | grep -q .; then
  echo "error: $BIN is all zeros; rebuild it" >&2; exit 1
fi
LOCAL_MD5=$(md5sum "$BIN" | cut -d' ' -f1)
echo "local  $LOCAL_MD5  $BIN ($(stat -c %s "$BIN") bytes)"

scp -q "$BIN" "$BRIDGE:$REMOTE"
REMOTE_MD5=$(ssh "$BRIDGE" "sync && md5sum $REMOTE | cut -d' ' -f1")
echo "bridge $REMOTE_MD5  $REMOTE"
if [ "$LOCAL_MD5" != "$REMOTE_MD5" ]; then
  echo "error: checksum mismatch after copy; NOT flashing" >&2
  ssh "$BRIDGE" "rm -f $REMOTE"
  exit 1
fi

if ssh "$BRIDGE" "fuser $PORT" >/dev/null 2>&1; then
  echo "error: something on the Pi has $PORT open:" >&2
  ssh "$BRIDGE" "fuser -v $PORT" >&2 || true
  echo "close it on the Pi (not just your local ssh), then retry" >&2
  ssh "$BRIDGE" "rm -f $REMOTE"
  exit 1
fi

trap 'ssh "$BRIDGE" "rm -f $REMOTE" >/dev/null 2>&1 || true' EXIT
ssh "$BRIDGE" "esptool --port $PORT --no-stub chip_id"
ssh "$BRIDGE" "esptool --port $PORT --no-stub write_flash $OFFSET $REMOTE"
echo "flashed $BIN at $OFFSET via $BRIDGE:$PORT"
