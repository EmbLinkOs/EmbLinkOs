#!/bin/sh
# Build QuickJS to RUN ON EmbLinkOS (build/js.elf -- JavaScript, hosted).
#
# Normally you do not run this: `make js` (or a plain `make`) builds it from
# $(QJS_SRC), and skips it silently when that source is absent -- the same
# bargain every other port here makes. This script exists to document what the
# port needs, and to apply the patch to a fresh tree.
#
#     tools/quickjs/build-quickjs-emblink.sh ~/cross/quickjs-2024-01-13
#
# WHY A PATCH AT ALL
# ------------------
# QuickJS is genuinely dependency-free C99 -- 58k lines cross-compiled against
# newlib with exactly TWO errors, both from optional POSIX:
#
#   PTHREAD_MUTEX_INITIALIZER   quickjs.c defines CONFIG_ATOMICS unless
#                               EMSCRIPTEN, and Atomics.* is SharedArrayBuffer
#                               across OS THREADS. A single-context engine has
#                               nothing to share with, and pthread_mutex was
#                               the only thing the file wanted from POSIX.
#   struct tm::tm_gmtoff        a BSD/GNU extension newlib does not carry.
#                               QuickJS ALREADY has a portable gmtime/mktime
#                               path for it, taken on _WIN32.
#
# The patch does not implement anything: it widens the two existing #ifdefs so
# a port can select the paths QuickJS already has. That is the whole diff, and
# it is why it is one patch and not a fork.
set -eu
SRC="${1:-$HOME/cross/quickjs-2024-01-13}"
HERE=$(cd "$(dirname "$0")" && pwd)
[ -f "$SRC/quickjs.c" ] || { echo "no quickjs.c in $SRC" >&2; exit 1; }
if grep -q CONFIG_NO_ATOMICS "$SRC/quickjs.c"; then
    echo "patch already applied"
else
    (cd "$SRC" && patch -p0 < "$HERE/0001-config-switches-for-a-freestanding-libc.patch")
fi
echo "now: make js   (QJS_SRC=$SRC)"
