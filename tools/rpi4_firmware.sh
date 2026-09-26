#!/bin/sh
# Fetch the Raspberry Pi 4 boot firmware into a cache directory -- docs/RPI4.md.
#
#   tools/rpi4_firmware.sh <cache-dir>
#
# The Pi 4 cannot boot anything without these: start4.elf is the VideoCore
# firmware that reads config.txt and loads kernel8.img, fixup4.dat its memory
# split, the .dtb the device tree the kernel reads its whole machine from, and
# disable-bt.dtbo the overlay that puts the PL011 on the header pins.
#
# They are Broadcom/Raspberry Pi binaries, redistributable but not ours, so
# they are fetched rather than committed -- from ONE pinned release, and each
# file is checked against a hash recorded here. A file that fails its check is
# deleted rather than kept: a boot partition assembled from an unexpected
# firmware is a debugging session about the wrong thing.
#
# To move to a newer release: change TAG, run this, and replace the hashes with
# the ones it reports as mismatched -- after booting the result on a real Pi.

set -eu

TAG=1.20260915
BASE="https://raw.githubusercontent.com/raspberrypi/firmware/$TAG/boot"

dir=${1:?usage: $0 <cache-dir>}
mkdir -p "$dir/overlays"

fail=0
check() {   # <relative path> <expected sha256>
    f=$1 want=$2
    if [ -f "$dir/$f" ] && [ "$(shasum -a 256 "$dir/$f" | cut -d' ' -f1)" = "$want" ]; then
        return 0
    fi
    echo "  FETCH    $f  (firmware $TAG)"
    if ! curl -fsSL -o "$dir/$f.part" "$BASE/$f"; then
        echo "rpi4_firmware: could not download $BASE/$f" >&2
        rm -f "$dir/$f.part"; fail=1; return 0
    fi
    got=$(shasum -a 256 "$dir/$f.part" | cut -d' ' -f1)
    if [ "$got" != "$want" ]; then
        echo "rpi4_firmware: $f: sha256 $got, expected $want -- discarded" >&2
        rm -f "$dir/$f.part"; fail=1; return 0
    fi
    mv "$dir/$f.part" "$dir/$f"
}

check start4.elf               560ead37044d849325755d3684ac1f74654b3ce6450604a42df6dd832cda9ede
check fixup4.dat               d181d5055daebac22513d6904fa9ce21d1e99b852cc391af095ade6f0b45c400
check bcm2711-rpi-4-b.dtb      75761b73c284e26623e4d1624bff13e67bce2ae620880efd81d6571a3739fcfb
check overlays/disable-bt.dtbo ea69d22dedc607fee75eec57d8a4cc0f0eab93cd75393e61a64c49fbac912d02

exit $fail
