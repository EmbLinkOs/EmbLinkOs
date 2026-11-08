# The userland on EmbCC's C library (`LIBC=emblibc`)

*How to build every EmbLinkOS program against EmbCC's `lib/libc` instead of
newlib, what is the same, what is different, and what is not built yet.*

## What it is

The userland's C library has been newlib, rebuilt for this OS
(`NEWLIB_PREFIX`), with `user/lib/syscalls.c` as its retargeting layer: the
POSIX names newlib calls, each written onto the kernel's system calls.

EmbCC's C library, `lib/libc` in the EmbCC tree, is one portable library
with one file per operating system underneath it. Its EmbLinkOS file,
`lib/libc/os/emblinkos/backend.c`, is the same retargeting layer rewritten
against that library's seam: the same system calls, the same translations
(absolute paths and a working directory kept in the library, the kernel's
open flags and `d_type` numbers, spawn with file actions in place of `fork`,
IPv4 sockets and the kernel's resolver, terminal control). Above it is the
rest of the library: ISO C complete, and of POSIX as much as this kernel can
carry -- descriptors, files and directories with their `*at` forms,
processes and `posix_spawn`, `popen`, `system`, signals to oneself, clocks,
`poll` and `select`, sockets and `getaddrinfo`, terminals and pseudo-terminals,
users and groups, time zones (`TZ` and the zone files) and `strptime`.

newlib stays the default. Nothing changes unless a build asks for it.

## Building

    make emblibc                     # builds the libraries for this tree
    make clean
    make LIBC=emblibc                # the whole userland on them

`EMBCC_ROOT` must be an EmbCC tree that has the whole library: main from
`7e4bb5b0` on (PRs #166-#168 brought POSIX, threads and TLS). A tree on a
branch from before them has no `<fcntl.h>` and no `libcxx-emblinkos` target,
and the build stops at the first program that includes it. `make clean` also
removes binaries staged into `build/` from elsewhere (`STAGED_APPS`, such as
`embcc.elf` and `embld.elf`); build without naming them, or stage them again.

`make emblibc` runs EmbCC's own `make libc-emblinkos libcxx-emblinkos`
against this tree's `user/lib` headers. It leaves, in the EmbCC tree,
`build/libc/emblinkos/` with `libc.a`, an empty `libm.a` (the math is in
`libc.a`) and `librt.a` (EmbCC's compiler runtime, with the unwinder C++'s
exceptions use), and `build/libcxx/emblinkos/libcxx.a`, EmbCC's C++ library.
`ARCH=aarch64` builds the `-aarch64` pair of each. `EMBCC_ROOT` says where
the EmbCC tree is; it defaults to `$HOME/EmbCC`.

The programs are still compiled by `x86_64-elf-gcc` (or `aarch64-elf-gcc`);
only the headers and the archive change. In `LIBC=emblibc` mode:

| | newlib (default) | `LIBC=emblibc` |
|---|---|---|
| Headers | newlib's, after `user/lib` | `lib/libc/include` first, then `user/lib` (whose `<time.h>`, `<termios.h>`, `<sys/*.h>` filled newlib's gaps and would otherwise shadow the library's), then the compiler's own; `-nostdinc` |
| Archive | `$(NEWLIB_PREFIX)/.../libc.a` | `$(EMBCC_ROOT)/build/libc/emblinkos/libc.a` |
| Retargeting | `user/lib/syscalls.c` | the library's backend; `syscalls.c` compiles to nothing (`EMBK_LIBC_EMBLIBC` is defined) |
| PIE | a second, `-fPIC` newlib | the same archive: EmbCC's x86-64 code is PC-relative, so every PIE target is a PIE |
| `crt0.c` | used | used, unchanged |

`build/.libc` records which library `build/` was compiled against. Make
cannot see a change of `LIBC`, and an object compiled against one library's
`<sys/stat.h>` linked with the other's `stat()` is a silent wrong layout, so a
different `LIBC` stops the build at once with `make clean` as the advice.

`build/syscalls.o` still exists in `LIBC=emblibc` mode, empty, and depends on
the library's archive. The rules that ask "is the libc newer than this
binary?" through it keep asking the right question.

## What is not built

- **The out-of-tree ports** -- CPython, git, tcc -- were configured against
  newlib and link it themselves. CPython and git are not built in
  `LIBC=emblibc` mode (`HAVE_PY`, `HAVE_GIT` are empty); tcc's binary is
  copied as it was built, never relinked, and still compiles against
  newlib's `/system/abi` on the OS. Each is a static newlib program, whole in
  itself.
- **C++ by `x86_64-elf-g++`**: that toolchain's libstdc++ was built against
  newlib, so `HAVE_CXX` is empty. C++ is built instead by EmbCC itself (next
  section).

## C++

Under `LIBC=emblibc` C++ needs no second toolchain. EmbCC compiles it
(`EMBCXX`), against EmbCC's C++ library over the same C library, and the
link is a C program's -- `crt0.o`, `newlib.ld`, gcc as the driver -- with
`-lcxx -lrt -lc` in front. `newlib-body.ld` brackets `.eh_frame` with
`__eh_frame_start`/`__eh_frame_end`, which EmbCC's unwinder scans: with
`-nostartfiles` nothing registers the tables for libgcc's.

Two programs are built this way: `cxxdemo` (`test cxx`, the floor:
constructors, `new`, guards, destructors at exit) and `cxxlib`
(`test cxxlib`: exceptions through frames that do not catch them, streams,
containers, smart pointers, `std::format`, `std::thread`, `std::filesystem`).

## What differs from newlib's layer

Mostly nothing a program can see: the backend was written from
`syscalls.c`, call by call. The differences are where the library does more:

- `getpass` reads a line with echo off (it was `ENOTTY`).
- `strptime`, `tzset`, `localtime` with a real `TZ`, `tm_gmtoff` and
  `tm_zone` (`QJS_CFLAGS`' `CONFIG_NO_TM_GMTOFF` is no longer needed).
- `<malloc.h>`'s `malloc_usable_size`, `mallinfo`, `malloc_stats`.
- `getline` is declared (mrsh no longer renames it to newlib's `__getline`).
- `<string.h>` brings `<strings.h>`, `<stdlib.h>` brings `<alloca.h>`, as
  glibc's do.
- `clock_getres(CLOCK_REALTIME)` is `{1, 0}`: a `timespec` is normalised.
  `syscalls.c` now says the same, and `posixdemo` checks for it.

And where this OS's rule is that a missing mechanism says so, the library
says so: `kill` of another process, `fork`, record locks, `mmap` of a file
are `ENOSYS`; `fcntl(F_SETFL)` with a flag it cannot set is `EINVAL`;
`sysconf(_SC_OPEN_MAX)` is the kernel's 64.

## Tested

`make LIBC=emblibc` builds every program in the tree (79 of the ELFs link
this library; the rest are freestanding). On QEMU, through
`tools/console_test.py`, 2026-10-30:

| Passes | |
|---|---|
| POSIX layer | `test posix`, `test env`, `test tz`, `test fd-sharing`, `test spawn-compat`, `test process-compat`, `test terminal`, `test usercopy`, `test pipe`, `test mtmalloc` |
| Shells | `test posix-shell` (mrsh, 338 checks), `test shell`, `test shellio`, `test shellscript`, `test extern`, `test externdiag` |
| On-OS toolchain | `test tcc`, `test tcc compile`, `test embbuild`, `test embbuild self` (fresh boot: it wants a clean build) |
| C++ | `test cxx`, `test cxxlib` |
| Threads | `test pthreads`: EmbCC's `tests/exec/posix-threads.c` (POSIX and C11 threads, mutexes, condition variables, read-write locks, barriers, semaphores, keys), the program glibc and the library both pass on Linux |

The desktop, 2026-11-06, both machines built `LIBC=emblibc` from EmbCC main
`7e4bb5b0` (every app's ELF carries `__emblibc_tls`):

| Machine | Passes |
|---|---|
| x86_64 under TCG | `contour`, `contour_shell`, `contour_controls`, `contour_keyboard`, `contour_modal`, `contour_windows`, `terminal`, `terminal_tabs`, `dock_motion`, `banner_motion`, `motion`, `overview`, `gesture`, `tile` and `workspace` smokes |
| arm64 under HVF | `gesture`, `motion`, `overview`, `tile` and `dock_motion` smokes |

`files_document_smoke` fails on its stale New Document click, as it does
under newlib (TODO.md). `workspace_smoke` failed until its symbol lookup
learnt that a static may share its name with one in the library (EmbCC's
`exit.c` has a `g_status`, as the Editor does): it read the library's. The
harness now takes the lowest address, the app's own objects being linked
first.

Not the library's, and the same under newlib:

- `test interrupt` wants exit 1; `shell/main.c` returns 130 (128 + SIGINT)
  for an interrupted program since the prompt-engine change.
- `test ctrlc2` waits for a real ^C; a scripted run never sends one.
- `test blockrace` reads a 9 MB `cxxdemo.elf` (the g++ build's), and
  `test shell cwd` runs git: neither port is on an image built here.

`errno` and `__thread` variables are per thread. `crt0.c` builds the first
thread's block -- from `__tls_geom` under `newlib.ld`, from the program
headers (`__ehdr_start`, PT_TLS) for the dynamically linked apps -- and
then fills in the library's `__emblibc_tls` with the template it used.
The library copies that template onto each new thread's stack before the
thread runs (EmbCC's `lib/libc/os/tls-template.h` is the layout's other
copy). Under newlib `__emblibc_tls` is undefined and nothing changes.

Open: `test fd-sharing`'s parallel readers fail now and then (5 of 29 runs
with this library). Caught with the reader instrumented: all four readers
read the 256 records the parent wrote and then go on into the previous
phase's writer records -- the parent's `open(O_TRUNC)` did not stick. The
independent writers just before open the same file themselves, and their
`process_reap` lines come after the parent's `waitpid` returned, so the
likely cause is a child's descriptor being released after its parent
truncated the file, restoring the old size. Both libraries make the same
system calls there; this one only changes the timing.

## Known gaps

- `fcntl` record locks, `mmap` of files, signals between processes: the
  kernel has none of them.
