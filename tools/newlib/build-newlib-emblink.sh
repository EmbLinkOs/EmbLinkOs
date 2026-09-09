#!/bin/sh
# Build newlib for EmbLinkOS, for EITHER target architecture.
#
#   usage: tools/newlib/build-newlib-emblink.sh /path/to/newlib-4.4.0.20231231 [target] [prefix]
#
#          target  x86_64-elf (default) | aarch64-elf
#          prefix  default $HOME/cross/newlib-c99          for x86_64-elf
#                          $HOME/cross/newlib-aarch64-c99  for aarch64-elf
#
# WHY THIS EXISTS AT ALL, on either architecture: the newlib baked into a stock
# cross toolchain is configured WITHOUT C99 printf formats, so "%zu" prints the
# literal "zu" and "%llu" is silently dropped. That is compiled into libc.a; no
# flag on the calling side can turn it back on. docs/BUILD_SETUP.md has the
# long version. This script is that document's recipe, made runnable and made
# to take the target as an argument -- ARM64.md phase A6 needs the same libc
# for aarch64, built the same way, and a second set of hand-typed configure
# flags is a second thing to get subtly wrong.
#
# The two options that are not about formats:
#   --disable-newlib-supplied-syscalls  we retarget newlib ourselves in
#       user/lib/syscalls.c against the EmbLink syscall ABI. newlib's own stubs
#       assume a different OS and would collide at link time.
#   --disable-multilib  one ABI per target. On aarch64 this also keeps newlib
#       from building the ILP32 variant, which we will never load.
set -eu

NLSRC="${1:?usage: $0 /path/to/newlib-x.y.z [target] [prefix]}"
[ -d "$NLSRC/newlib" ] || { echo "$0: $NLSRC is not a newlib source tree" >&2; exit 2; }
NLSRC=$(cd "$NLSRC" && pwd)

TARGET="${2:-x86_64-elf}"
case "$TARGET" in
  x86_64-elf)  DEFPREFIX="$HOME/cross/newlib-c99" ;;
  aarch64-elf) DEFPREFIX="$HOME/cross/newlib-aarch64-c99" ;;
  *) echo "$0: unknown target '$TARGET' (expected x86_64-elf or aarch64-elf)" >&2; exit 2 ;;
esac
PREFIX="${3:-$DEFPREFIX}"

command -v "$TARGET-gcc" >/dev/null 2>&1 || {
  echo "$0: $TARGET-gcc is not on PATH -- install the cross toolchain first" >&2; exit 2; }

# --- the one source patch, and only for aarch64 ---------------------------
# newlib's configure.host gives x86_64-elf and aarch64-elf DIFFERENT syscall
# contracts, and the difference is invisible until a program fails to link.
#
#   x86_64-elf   falls through to the default `*)` case:
#                  syscall_dir=            (libc supplies no syscall layer)
#                  -DMISSING_SYSCALL_NAMES (so <_syslist.h> maps _write -> write)
#   aarch64-*-*  has its OWN case, which does neither:
#                  syscall_dir=syscalls    (libc DEFINES write(), calling _write_r)
#                  no MISSING_SYSCALL_NAMES (so _write_r calls _write)
#
# We supply the syscall layer ourselves in user/lib/syscalls.c, and it defines
# the BARE POSIX names -- write(), read(), open() -- because that is what the
# x86 libc has always asked for. Against a stock aarch64 newlib every one of
# them is the wrong name: the link dies on `undefined reference to _write' for
# ten functions, and libc's own write() sits in the archive waiting to collide.
#
# So make aarch64 behave exactly like x86_64. This is a one-line-equivalent
# change to a THIRD-PARTY tree, applied here rather than documented as a manual
# step, for the same reason tools/tcc and tools/cpython apply theirs: a patch a
# human has to remember is a patch that is sometimes not applied. It is
# idempotent -- re-running this script on an already-patched tree is a no-op.
if [ "$TARGET" = "aarch64-elf" ]; then
  CH="$NLSRC/newlib/configure.host"
  if grep -q 'EMBLINK: aarch64 uses the default syscall contract' "$CH"; then
    echo "==> configure.host already patched"
  else
    awk '
      /^  aarch64\*-\*-\*\)/ { inblk = 1 }
      inblk && /^\tsyscall_dir=syscalls$/ {
          print "\t# EMBLINK: aarch64 uses the default syscall contract (see";
          print "\t# tools/newlib/build-newlib-emblink.sh) -- no libc-supplied";
          print "\t# syscall layer, and _write/_read/... spelled as write/read/...";
          print "\tsyscall_dir=";
          print "\tnewlib_cflags=\"${newlib_cflags} -DMISSING_SYSCALL_NAMES\"";
          patched = 1;
          next
      }
      inblk && /^\t;;/ { inblk = 0 }
      { print }
      END { if (!patched) { print "PATCH-FAILED" > "/dev/stderr"; exit 3 } }
    ' "$CH" > "$CH.embk" || { echo "$0: could not patch $CH" >&2; exit 3; }
    mv "$CH.embk" "$CH"
    echo "==> patched configure.host for aarch64"
  fi
fi

BUILD="${BUILD_DIR:-${TMPDIR:-/tmp}/build-newlib-$TARGET}"
rm -rf "$BUILD"; mkdir -p "$BUILD"

echo "==> newlib $NLSRC"
echo "    target $TARGET  ->  $PREFIX"
echo "    build  $BUILD"

cd "$BUILD"
"$NLSRC/configure" --target="$TARGET" --prefix="$PREFIX" \
    --disable-newlib-supplied-syscalls --disable-multilib --disable-nls \
    --enable-newlib-io-c99-formats --enable-newlib-io-long-long

make ${MAKEFLAGS:--j8}
make install

# The claim this script makes, checked rather than assumed. A libc.a in the
# right place proves the install ran; it does not prove C99 formats survived
# configure, and that is the entire reason for the rebuild.
LIB="$PREFIX/$TARGET/lib/libc.a"
[ -f "$LIB" ] || { echo "$0: no $LIB -- the install did not produce a libc" >&2; exit 1; }
# The syscall contract, checked rather than assumed -- this is the thing that
# differed silently between the two targets. libc must ASK for the bare POSIX
# names (user/lib/syscalls.c supplies them) and must not SUPPLY them itself.
if "$TARGET-nm" "$LIB" 2>/dev/null | grep -qw "U _write"; then
  echo "$0: $LIB calls _write, not write -- the configure.host patch did not take." >&2
  echo "     user/lib/syscalls.c defines write(); this libc would not link." >&2
  exit 1
fi
if "$TARGET-nm" "$LIB" 2>/dev/null | grep -qw "T write"; then
  echo "$0: $LIB DEFINES write() -- libc's own syscall layer was built." >&2
  echo "     It would collide with user/lib/syscalls.c." >&2
  exit 1
fi
echo
echo "==> ok: $LIB  (asks for write(), does not define it)"
echo "    point the build at it with:"
case "$TARGET" in
  x86_64-elf)  echo "      make NEWLIB_PREFIX=$PREFIX" ;;
  aarch64-elf) echo "      make ARCH=aarch64 NEWLIB_AARCH64_PREFIX=$PREFIX" ;;
esac
