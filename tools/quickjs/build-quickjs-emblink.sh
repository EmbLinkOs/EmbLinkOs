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
#   malloc_usable_size          quickjs.c includes <malloc.h> only #ifdef
#                               __linux__, but our TARGET is x86_64-elf, so the
#                               header never came in and the call below it was
#                               an implicit declaration. newlib declares the
#                               function -- only the #include was unreachable.
#                               gcc <=13 let this through as a warning, which is
#                               why it surfaced only on a newer host compiler
#                               (gcc 14+ makes implicit declarations an error).
#
# The patch does not implement anything: it widens the two existing #ifdefs so
# a port can select the paths QuickJS already has. That is the whole diff, and
# it is why it is one patch and not a fork.
set -eu
SRC="${1:-$HOME/cross/quickjs-2024-01-13}"
HERE=$(cd "$(dirname "$0")" && pwd)
[ -f "$SRC/quickjs.c" ] || { echo "no quickjs.c in $SRC" >&2; exit 1; }
if grep -q CONFIG_MALLOC_H "$SRC/quickjs.c"; then
    echo "patch already applied"
else
    (cd "$SRC" && patch -p0 -N -r - < "$HERE/0001-config-switches-for-a-freestanding-libc.patch")
    grep -q CONFIG_MALLOC_H "$SRC/quickjs.c" || {
        echo "$0: patch did not take -- $SRC/quickjs.c is not stock 2024-01-13?" >&2
        exit 1
    }
fi
echo "now: make js   (QJS_SRC=$SRC)"
