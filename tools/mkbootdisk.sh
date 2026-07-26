#!/usr/bin/env bash
#
# mkbootdisk.sh -- assemble ONE bootable medium that carries the whole system:
# the boot code + kernel in the MBR gap, and an EMBKFS volume in partition 1.
#
# Why one image instead of the two the IDE path uses (myos.img + embkfs.img on
# two -drive lines): a real USB stick, or a single physical disk, is ONE device.
# `dd if=<out> of=/dev/sdX` gives a stick you can boot on real hardware, and the
# same file boots in QEMU behind qemu-xhci (`make run-usb`). The kernel finds the
# EMBKFS by probing every block device for the superblock (embkfs_init), and the
# partition scanner (embk_partition_scan_all) exposes partition 1 as a block
# device -- so nothing kernel-side is hardcoded to a drive index.
#
# Layout (512-byte sectors):
#   LBA 0            stage1 (MBR: boot code 0..439, partition table 446..509)
#   LBA 1 .. 8       stage2
#   LBA 9 .. K       kernel   (stage2 reads the kernel ELF from LBA 9)
#   LBA PART_START   partition 1, type 0x83 -- the EMBKFS image, verbatim
#
# PART_START is aligned up past the kernel to a 2048-sector (1 MB) boundary, so
# the EMBKFS partition never overlaps the boot code no matter how the kernel grows.
#
# Usage: mkbootdisk.sh <stage1.bin> <stage2.bin> <kernel.bin> <embkfs.img> <out.img>
set -euo pipefail

if [ "$#" -ne 5 ]; then
    echo "usage: $0 <stage1.bin> <stage2.bin> <kernel.bin> <embkfs.img> <out.img>" >&2
    exit 2
fi
STAGE1=$1 STAGE2=$2 KERNEL=$3 EMBKFS=$4 OUT=$5

for f in "$STAGE1" "$STAGE2" "$KERNEL" "$EMBKFS"; do
    [ -f "$f" ] || { echo "mkbootdisk: missing input '$f'" >&2; exit 1; }
done

# A fixed MBR disk id (offset 440, little-endian). Deterministic on purpose --
# a fresh random id every build would churn the image and defeat reproducibility.
# The kernel does NOT need this exact value (it matches the boot device by the
# signature stage2 captures, whatever it is); a stable one is just hygiene.
DISK_ID="0x454D424B"   # 'EMBK'

sec() { echo $(( ( $(stat -c%s "$1") + 511 ) / 512 )); }
roundup() { echo $(( ( ($1 + $2 - 1) / $2 ) * $2 )); }

S1=$(sec "$STAGE1"); S2=$(sec "$STAGE2"); KS=$(sec "$KERNEL"); ES=$(sec "$EMBKFS")

# stage2 hardcodes the kernel at LBA 9, and loads exactly S2 sectors of stage2
# from LBA 1. Enforce the invariant the layout depends on rather than silently
# producing an unbootable disk: stage1 = 1 sector, stage2 must fit in LBA 1..8.
if [ "$S1" -ne 1 ]; then echo "mkbootdisk: stage1 is $S1 sectors, expected 1" >&2; exit 1; fi
if [ "$S2" -gt 8 ]; then echo "mkbootdisk: stage2 is $S2 sectors, > 8 (kernel starts at LBA 9)" >&2; exit 1; fi

BOOT_SECTORS=$(( 9 + KS ))                       # boot code + kernel occupy LBA 0..(9+KS-1)
PART_START=$(roundup "$BOOT_SECTORS" 2048)       # 1 MB-aligned, clear of the kernel
[ "$PART_START" -ge 2048 ] || PART_START=2048
TOTAL=$(( PART_START + ES ))

echo "mkbootdisk: kernel $KS sec (boot ends LBA $BOOT_SECTORS); EMBKFS partition at LBA $PART_START, $ES sec; total $TOTAL sec ($(( TOTAL/2048 )) MB)"

# 1) blank image of the exact size
dd if=/dev/zero of="$OUT" bs=512 count="$TOTAL" status=none

# 2) boot code + kernel into the gap (stage1 | stage2 | kernel, contiguous from LBA 0)
cat "$STAGE1" "$STAGE2" "$KERNEL" | dd of="$OUT" bs=512 conv=notrunc status=none

# 3) MBR partition table (preserves the boot code at 0..439; writes id at 440,
#    table at 446, signature at 510). Bootable flag set so a picky BIOS is happy.
printf 'label: dos\nlabel-id: %s\nstart=%d, type=83, bootable\n' "$DISK_ID" "$PART_START" \
    | sfdisk --no-reread --no-tell-kernel "$OUT" >/dev/null

# 4) EMBKFS image verbatim into partition 1 (partition-relative LBA 0 = superblock)
dd if="$EMBKFS" of="$OUT" bs=512 seek="$PART_START" conv=notrunc status=none

echo "mkbootdisk: wrote $OUT"
