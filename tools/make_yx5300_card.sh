#!/bin/bash
# Format a YX5300 SD card blank (FAT32) and copy a staged folder's 001.mp3, 002.mp3 ...
# onto it IN ORDER, one at a time (the YX5300 plays by copy order, not by name).
#
#   sudo bash tools/make_yx5300_card.sh <FS-UUID of the card> "<staged folder>" <LABEL> [whole]
#   whole = wipe the card's partition table and make ONE FAT32 partition first
#           (for cards that came with several partitions, e.g. an old Pi image;
#           the YX5300 wants a single plain FAT32 partition).
#   e.g. sudo bash tools/make_yx5300_card.sh B7F7-9883 \
#          "~/Desktop/Spatial audio/SD cards (ready to copy)/Patch Left (Near Zombie barrel)" PATCHLEFT
#
# Safety: the card is found by its filesystem UUID (lsblk), and the script refuses
# anything that isn't a removable disk under 64 GB, so it can't hit the backup,
# media or system drives. Written 2026-10-01 for the Patch cards.
set -euo pipefail
UUID="${1:?FS UUID}"; SRC="${2:?staged folder}"; LABEL="${3:-YX5300}"; MODE="${4:-}"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -d "$SRC" ] || { echo "no folder: $SRC"; exit 1; }
PART=$(blkid -U "$UUID") || { echo "no partition with UUID $UUID"; exit 1; }
DISK=/dev/$(lsblk -no PKNAME "$PART")
RM=$(lsblk -dno RM "$DISK" | tr -d " "); SIZE=$(lsblk -bdno SIZE "$DISK" | tr -d " ")   # lsblk pads with spaces
echo "card: $PART on $DISK  removable=$RM  size=$((SIZE/1000000000)) GB"
[ "$RM" = 1 ] || { echo "REFUSED: $DISK is not removable"; exit 1; }
[ "$SIZE" -lt 64000000000 ] || { echo "REFUSED: $DISK is 64 GB or larger"; exit 1; }
# Unmount every partition of the card; if anything still holds one (a file
# manager window showing the card does), stop with a clear message instead of
# hitting "Device or resource busy" halfway through (2026-10-01).
unmount_all() {
  for p in $(lsblk -lnpo NAME "$DISK" | tail -n +2); do umount "$p" 2>/dev/null || true; done
  if grep -q "^$DISK" /proc/mounts; then
    echo "STOPPED: the card is still mounted (close any file manager window showing it, then re-run):"
    grep "^$DISK" /proc/mounts | cut -d" " -f1,2
    exit 1
  fi
}
mapfile -t FILES < <(cd "$SRC" && ls [0-9][0-9][0-9].mp3 | sort)
[ "${#FILES[@]}" -gt 0 ] || { echo "no 001.mp3... in $SRC"; exit 1; }
echo "will copy, in order: ${FILES[*]}"

if [ "$MODE" = whole ]; then
  echo "whole-card mode: replacing the partition table on $DISK with one FAT32 partition"
  unmount_all
  wipefs -a "$DISK"
  echo 'label: dos
type=c' | sfdisk "$DISK"
  partprobe "$DISK"; sleep 2
  PART=$(lsblk -lnpo NAME "$DISK" | sed -n 2p)
  [ -b "$PART" ] || { echo "no partition after sfdisk"; exit 1; }
  unmount_all                                        # the desktop may auto-mount the new partition
else
  unmount_all
fi
mkfs.vfat -F 32 -n "$LABEL" "$PART"
MNT=$(mktemp -d)
mount -o uid="${SUDO_UID:-1000}",gid="${SUDO_GID:-1000}" "$PART" "$MNT"
for f in "${FILES[@]}"; do
  cp "$SRC/$f" "$MNT/$f"; sync
  echo "  copied $f"
done
echo "--- card contents (directory order = play order) ---"
ls -lU "$MNT"
for f in "${FILES[@]}"; do
  cmp -s "$SRC/$f" "$MNT/$f" && echo "  verified $f" || { echo "  !! MISMATCH $f"; exit 1; }
done
umount "$MNT"; rmdir "$MNT"
echo "DONE: card unmounted, safe to remove"
