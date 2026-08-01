#!/bin/bash
# Configure CPython for EmbLinkOS. Reproducible invocation -- see also
# memory/cpython-port.md for WHY each piece is here.
#
#   usage: tools/cpython/configure-py-emblink.sh /path/to/Python-3.14.6 [cfg args]
#
# APPLIES tools/cpython/0001-newlib-timezone-and-clockid.patch first: CPython
# does NOT build for newlib unmodified (it has no `timezone`/`daylight`, and its
# clockid_t is long-sized). Idempotent.
set -euo pipefail

SRC="${1:?usage: $0 /path/to/Python-3.14.6 [configure args...]}"; shift || true
# Paths are DERIVED, never hardcoded -- this script must work on a teammate's
# checkout. MYOS comes from where this script lives; NEWLIB_PREFIX from the
# Makefile (the ONE source of truth: `make print-newlib-prefix`); the source
# tree is an argument. Same convention as tools/tcc/build-tcc-emblink.sh.
HERE=$(cd "$(dirname "$0")" && pwd)
M="${MYOS:-$(cd "$HERE/../.." && pwd)}"
NEWLIB_PREFIX="${NEWLIB_PREFIX:-$(make -C "$M" -s --no-print-directory print-newlib-prefix)}"
NL="$NEWLIB_PREFIX/x86_64-elf"        # the SAME libc the OS's apps link
export PATH="${CROSS_BIN:-/usr/local/cross/bin}:$PATH"   # target binutils (as/ld)
BLD="${PY_BUILD:-$SRC/../build-py}"

# The patch is REQUIRED, not optional; see the header. Idempotent.
if grep -q '__NEWLIB__' "$SRC/Modules/timemodule.c"; then
    echo "  patch    already applied"
else
    echo "  PATCH    0001-newlib-timezone-and-clockid"
    patch -p1 -d "$SRC" < "$HERE/0001-newlib-timezone-and-clockid.patch"
fi

# The OS must have been built at least once: we borrow its crt0/syscall glue so
# that configure's link tests can actually produce an executable. Without these,
# a bare link is "ld: cannot find crt0.o" and configure dies at check #1 with
# "C compiler cannot create executables".
for o in "$M/build/crt0.o" "$M/build/syscalls.o"; do
    [ -f "$o" ] || { echo "missing $o -- run 'make' in $M first" >&2; exit 1; }
done

mkdir -p "$BLD"; cd "$BLD"

# SSIZE_MAX: newlib does not define it at all, but pyport.h requires it. Proven
# by _Static_assert that ssize_t is exactly signed long on this ABI, so the
# value is LONG_MAX; spelled as a literal so it needs no <limits.h> first.
#
# -I$M/user/lib: puts EmbLink's OVERRIDE headers ahead of newlib's, exactly the
# mechanism newlib documents ("this file is overridden by ... in libc/sys/.../sys").
# It supplies user/lib/sys/dirent.h -- newlib's is a bare `#error "<dirent.h> not
# supported"` -- and user/lib/sys/utime.h, replacing a "dummy" newlib header that
# is broken (uses time_t without including it, and never declares utime()).
# Must precede -isystem, since -I is searched first.
#
# _POSIX_TIMERS / _POSIX_MONOTONIC_CLOCK: newlib only defines these for targets
# it knows (RTEMS/Cygwin), so on bare x86_64-elf <time.h> hides clock_gettime()
# and CLOCK_MONOTONIC and NO feature-test macro reveals them. EmbLink's libc
# implements both clocks (user/lib/syscalls.c), so we assert the macros here to
# match reality. configure's link test then finds clock_gettime in the
# syscalls.o we pass via LDFLAGS and defines HAVE_CLOCK_GETTIME, which is what
# gates the _PyTime_AsTimespec* declarations that Python/parking_lot.c needs.
# MODULE_BUILDTYPE=static: build every stdlib extension module INTO the binary
# rather than as a .cpython-314.so. EmbLink has no dlopen (configure agrees --
# it leaves HAVE_DYNAMIC_LOADING undefined and picks DYNLOADFILE=dynload_stub.o;
# our in-kernel 2-object loader is not a dlopen), but CPython keys this default
# off `host_cpu` being wasm32/wasm64 rather than off dynamic loading, so plain
# x86_64 wrongly defaults to "shared" and then links .so files with the HOST ld.
# configure honours the env var (MODULE_BUILDTYPE=${MODULE_BUILDTYPE:-shared}),
# so this needs no patch, and it takes the same branch WASI does.
CONFIG_SITE="$HERE/emblink-config.site" \
MODULE_BUILDTYPE=static \
"$SRC"/configure \
  --host=x86_64-emblink --build=x86_64-pc-linux-gnu \
  --with-build-python=python3 \
  --disable-shared --disable-ipv6 --without-ensurepip \
  CC=x86_64-elf-gcc AR=x86_64-elf-ar RANLIB=x86_64-elf-ranlib \
  CFLAGS="-mno-red-zone -fno-stack-protector -O2 -I$M/user/lib -isystem $NL/include \
          -DSSIZE_MAX=0x7fffffffffffffffL \
          -D_POSIX_TIMERS=200809L -D_POSIX_MONOTONIC_CLOCK=200809L" \
  LDFLAGS="-static -nostartfiles -T $M/user/lib/newlib.ld -L$NL/lib $M/build/crt0.o $M/build/syscalls.o" \
  "$@" 2>&1 | tee cfg.log

# --- POST-CONFIGURE: enable networking (the pip/git-over-HTTP foundation) -----
# A cross-compile can't RUN link probes, so configure leaves every socket-layer
# feature macro #undef and _socket falls back to a stub whose socket() just sets
# errno=ENOTSUP. EmbLink's newlib libc DOES provide the full BSD surface
# (user/lib/syscalls.c over the kernel CAP_NETWORK layer -- socket/connect/bind/
# listen/accept/recvfrom/sendto + getaddrinfo/getnameinfo/inet_*), so we assert
# the matching HAVE_* here to match reality. Idempotent (flips only the #undef
# form). Without HAVE_SOCKET the interpreter builds but `socket.socket()` raises
# OSError(ENOTSUP); the method-table guards (HAVE_CONNECT/BIND/...) drop the
# corresponding methods entirely. Verified live: `test python net` -> HTTP 200.
PYCONF="$BLD/pyconfig.h"
for m in HAVE_SYS_SOCKET_H HAVE_NETINET_IN_H HAVE_NETINET_TCP_H HAVE_NETDB_H \
         HAVE_ARPA_INET_H HAVE_SOCKET HAVE_CONNECT HAVE_BIND HAVE_LISTEN \
         HAVE_ACCEPT HAVE_RECVFROM HAVE_SENDTO HAVE_GETADDRINFO HAVE_GETNAMEINFO \
         HAVE_GAI_STRERROR HAVE_ADDRINFO HAVE_SOCKADDR_STORAGE HAVE_SOCKLEN_T \
         HAVE_GETHOSTBYNAME HAVE_INET_PTON HAVE_INET_NTOP; do
    sed -i "s|/\* #undef $m \*/|#define $m 1|" "$PYCONF"
done
# socketmodule.c is not in the default static set; declare it as a builtin.
touch "$BLD/Modules/Setup.local"
grep -q '^_socket socketmodule.c' "$BLD/Modules/Setup.local" \
    || echo '_socket socketmodule.c' >> "$BLD/Modules/Setup.local"
echo "  NET      pyconfig.h socket macros + Setup.local _socket asserted"

# _embtls: HTTPS without OpenSSL. A tiny C module (tools/cpython/_embtlsmodule.c)
# wrapping our own TLS 1.3 client (user/lib/tls, docs/TLS.md) over a socket fd,
# so http.client/urllib -- and pip -- can do authenticated https://. makesetup
# derives the .o path as Modules/<basename> and only resolves sources under
# Modules/, so the source is SYMLINKED in; the prebuilt libtls objects
# ($M/build/tls_*.o, made by the OS build) are listed for the static link.
ln -sf "$M/tools/cpython/_embtlsmodule.c" "$SRC/Modules/_embtlsmodule.c"
TLSOBJS=$(ls "$M"/build/tls_*.o 2>/dev/null | tr '\n' ' ')
if [ -n "$TLSOBJS" ]; then
    grep -q '^_embtls ' "$BLD/Modules/Setup.local" \
        || echo "_embtls _embtlsmodule.c -I$M/user/lib/tls $TLSOBJS" >> "$BLD/Modules/Setup.local"
    echo "  TLS      _embtls declared ($(echo "$TLSOBJS" | wc -w) libtls objects)"
else
    echo "  TLS      SKIP _embtls -- no $M/build/tls_*.o (run 'make' in the OS first)" >&2
fi

echo
echo "=== configure OK: MACHDEP=$(grep -E '^MACHDEP=' Makefile | head -1) ==="
echo "=== next: cd $BLD && make -j4 ==="
