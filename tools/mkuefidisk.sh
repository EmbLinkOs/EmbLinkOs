#!/usr/bin/env bash
#
# mkuefidisk.sh -- build a GPT disk with an EFI System Partition holding the
# loader at the firmware-standard removable path /EFI/BOOT/BOOTX64.EFI.
#
# The kernel is embedded IN the .efi (uefi_kernel_blob.o), so the ESP carries
# just the loader. With an optional third argument, a SECOND GPT partition is
# added carrying a raw EMBKFS image -- this makes a single, self-contained USB
# stick: the firmware launches BOOTX64.EFI, the embedded kernel comes up, then
# embkfs_init() probes every block device (the ESP disk's partitions included)
# and mounts the EMBKFS partition as root. No separate root drive needed.
#
# Usage: mkuefidisk.sh <BOOTX64.EFI> <out.img> [embkfs.img]
#   two args   -> ESP-only disk (kernel + loader; root supplied elsewhere)
#   three args -> ESP + EMBKFS disk (a real bootable single-device stick)
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 <BOOTX64.EFI> <out.img> [embkfs.img]" >&2
    exit 2
fi
EFI=$1 OUT=$2 EMBKFS=${3:-}
[ -f "$EFI" ] || { echo "mkuefidisk: missing '$EFI'" >&2; exit 1; }
[ -z "$EMBKFS" ] || [ -f "$EMBKFS" ] || { echo "mkuefidisk: missing '$EMBKFS'" >&2; exit 1; }

ESP_MB=64
START=2048                                   # 1 MiB-aligned first partition
ESP_SECTORS=$(( ESP_MB * 1024 * 1024 / 512 ))

# ESP GUID (C12A7328-...) for partition 1; Linux-filesystem-data GUID for the
# EMBKFS partition. The kernel's gpt_scan registers EVERY partition regardless
# of type GUID and embkfs_init probes each for a superblock, so the type here is
# advisory only -- a sane, non-ESP value keeps other tools from touching it.
ESP_TYPE=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
DATA_TYPE=0FC63DAF-8483-4772-8E79-3D69D8477DE4

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAT="$TMP/esp.fat"

# 1) a standalone FAT32 filesystem image, then the /EFI/BOOT tree + the loader
dd if=/dev/zero of="$FAT" bs=512 count="$ESP_SECTORS" status=none
mkfs.vfat -F 32 -n EMBLINKEFI "$FAT" >/dev/null
mmd   -i "$FAT" ::/EFI ::/EFI/BOOT
mcopy -i "$FAT" "$EFI" ::/EFI/BOOT/BOOTX64.EFI

if [ -z "$EMBKFS" ]; then
    # --- ESP-only disk ---------------------------------------------------------
    TOTAL=$(( START + ESP_SECTORS + 2048 ))       # + slack for the backup GPT
    dd if=/dev/zero of="$OUT" bs=512 count="$TOTAL" status=none
    printf 'label: gpt\nstart=%d, size=%d, type=%s, name="EFI System"\n' \
        "$START" "$ESP_SECTORS" "$ESP_TYPE" | \
        sfdisk --no-reread --no-tell-kernel "$OUT" >/dev/null
    dd if="$FAT" of="$OUT" bs=512 seek="$START" conv=notrunc status=none
    echo "mkuefidisk: wrote $OUT (GPT + ESP with /EFI/BOOT/BOOTX64.EFI)"
    exit 0
fi

# --- ESP + EMBKFS disk (single self-contained device) --------------------------
# EMBKFS partition: 1 MiB-aligned right after the ESP, sized to the image
# (rounded up to a whole sector). START2 = START + ESP_SECTORS is already a
# multiple of 2048 (64 MiB ESP), so alignment holds.
EMBKFS_BYTES=$(stat -c%s "$EMBKFS")
EMBKFS_SECTORS=$(( (EMBKFS_BYTES + 511) / 512 ))
START2=$(( START + ESP_SECTORS ))
TOTAL=$(( START2 + EMBKFS_SECTORS + 2048 ))       # + slack for the backup GPT

dd if=/dev/zero of="$OUT" bs=512 count="$TOTAL" status=none
printf 'label: gpt\nstart=%d, size=%d, type=%s, name="EFI System"\nstart=%d, size=%d, type=%s, name="EMBKFS root"\n' \
    "$START"  "$ESP_SECTORS"    "$ESP_TYPE" \
    "$START2" "$EMBKFS_SECTORS" "$DATA_TYPE" | \
    sfdisk --no-reread --no-tell-kernel "$OUT" >/dev/null

# 3) drop the FAT into partition 1 and the raw EMBKFS into partition 2
dd if="$FAT"    of="$OUT" bs=512 seek="$START"  conv=notrunc status=none
dd if="$EMBKFS" of="$OUT" bs=512 seek="$START2" conv=notrunc status=none

echo "mkuefidisk: wrote $OUT (GPT: ESP /EFI/BOOT/BOOTX64.EFI + EMBKFS root, single device)"
