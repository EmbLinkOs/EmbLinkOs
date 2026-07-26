#!/usr/bin/env bash
#
# mkuefidisk.sh -- build a GPT disk with an EFI System Partition holding the
# loader at the firmware-standard removable path /EFI/BOOT/BOOTX64.EFI.
#
# For the M1 MVP the kernel is embedded IN the .efi, so the ESP carries just the
# loader; the kernel's EMBKFS root is supplied on a separate drive (see the
# run-uefi target). Later this grows a second (EMBKFS) partition and moves the
# kernel to a file on the ESP.
#
# Usage: mkuefidisk.sh <BOOTX64.EFI> <out.img>
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <BOOTX64.EFI> <out.img>" >&2
    exit 2
fi
EFI=$1 OUT=$2
[ -f "$EFI" ] || { echo "mkuefidisk: missing '$EFI'" >&2; exit 1; }

ESP_MB=64
START=2048                                   # 1 MiB-aligned first partition
ESP_SECTORS=$(( ESP_MB * 1024 * 1024 / 512 ))

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAT="$TMP/esp.fat"

# 1) a standalone FAT32 filesystem image, then the /EFI/BOOT tree + the loader
dd if=/dev/zero of="$FAT" bs=512 count="$ESP_SECTORS" status=none
mkfs.vfat -F 32 -n EMBLINKEFI "$FAT" >/dev/null
mmd   -i "$FAT" ::/EFI ::/EFI/BOOT
mcopy -i "$FAT" "$EFI" ::/EFI/BOOT/BOOTX64.EFI

# 2) a GPT disk, one EFI System Partition (type C12A7328-... = the ESP GUID)
TOTAL=$(( START + ESP_SECTORS + 2048 ))       # + slack for the backup GPT
dd if=/dev/zero of="$OUT" bs=512 count="$TOTAL" status=none
printf 'label: gpt\nstart=%d, size=%d, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="EFI System"\n' \
    "$START" "$ESP_SECTORS" | sfdisk --no-reread --no-tell-kernel "$OUT" >/dev/null

# 3) drop the FAT filesystem into the partition
dd if="$FAT" of="$OUT" bs=512 seek="$START" conv=notrunc status=none

echo "mkuefidisk: wrote $OUT (GPT + ESP with /EFI/BOOT/BOOTX64.EFI)"
