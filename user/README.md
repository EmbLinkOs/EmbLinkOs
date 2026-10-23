# EmbLink userland: the SDK + newlib port

Two complementary layers let a program use the full C library *and* the
OS-native primitives POSIX can't express. If you're building a *graphical*
app rather than a command-line program, read
[docs/EMUI_GUIDE.md](../docs/EMUI_GUIDE.md) instead — it covers the third
build mode below (dynamic linking against `libembk.so`) in the context of
actually writing a UI app; this file is the lower-level reference.

## Where the programs live

One directory per program, and the CATEGORY FOLDER IS THE CLASSIFICATION —
there is no list anywhere else saying what a thing is:

| Directory | What lives there |
|-----------|------------------|
| `user/apps/<name>/` | applications with a user interface |
| `user/system/<name>/` | what makes the machine boot and log in (`init`, `login`, `setup`) |
| `user/tools/<name>/` | command-line programs |
| `user/tests/<name>/` | witnesses the kernel's self-tests spawn |
| `user/examples/<name>/` | reference programs [the toolkit guide](../docs/EMUI_GUIDE.md) points at |

A program's directory holds everything belonging to it: the source, its
declared namespace (`.ns`), its capability manifest (`.caps`), its launcher
entry (`.app`), its build manifest (`.build.ebm`). Nothing else in the tree
needs to know where a program is — the Makefile's `$(call USER_SRC,<name>)`
turns a name into a path, and moving a program between categories is moving
its folder.

Everything else under `user/` is a shared library rather than a program:
`user/lib/` is the SDK and the newlib port described below, and `user/web/`,
`user/note/`, `user/photos/`, `user/audio/`, `user/git/`, `user/httpd/`,
`user/pkg/` and `user/emlibc/` are components more than one program draws on
(the browser and the photo viewer share `user/web`'s image decoders, for
instance).

## Layers

| File | What it is | Who uses it |
|------|-----------|-------------|
| `embk_syscall.h` | Raw `int 0x80` ABI: syscall numbers + register wrappers (`embk_syscall0..6`) + the `[-4095,-1]` error-range helper. Hand-synced to the kernel's `syscall.c` dispatch table. | Everything below. |
| `embk.h` | **The EmbLink SDK.** Header-only, typed native primitives: `embk_spawn`, `embk_thread_create/join/exit`, `embk_wait/kill`, `embk_yield`, `embk_readdir`, `embk_stat`, file-actions, window/compositor calls (`embk_win_create_shared_ex`, `embk_win_move`, `embk_win_resize`, ...). Raw kernel return convention (`ret < 0` is `-EMBK_*`, no errno). | Freestanding, static-newlib, **and** dynamically-linked programs. |
| `crt0.c` | newlib entry stub: aligns the stack for a raw ELF entry, calls `main`, `exit`s. | newlib programs only (both static and dynamic). |
| `syscalls.c` | **The newlib retargeting layer.** POSIX stubs (`read`, `write`, `open`, `sbrk`, `fstat`, `gettimeofday`, …) that back newlib's libc; set `errno`, return `-1` on failure. | newlib programs only (both static and dynamic). |

## Three ways to build a program

**Freestanding** (no libc — smallest, e.g. `init.c`): write your own `_start`,
`#include "embk.h"`, link just your `.o` with `user.ld`. You get spawn,
threads, sbrk, VFS — no printf/malloc.

**Static newlib** (full libc, e.g. `hello.c`): `#include <stdio.h>` etc. for
libc and `#include "embk.h"` for native primitives. Link `crt0.o +
syscalls.o + yourprog.o -lc -lgcc` with `newlib.ld` (`-static -nostartfiles
-T newlib.ld`). You get printf/malloc/string/time **and** spawn/threads.
Good for a self-contained command-line tool.

**Dynamic, against `libembk.so`** (every EmUI/GUI app — `uidemo.elf`,
`v4demo.elf`, `home.elf`, ...): same `#include`s as static newlib, plus the
EmUI headers (`"ui.h" "em.h" "theme.h"`). Link `crt0.o + syscalls.o +
yourprog.o + libembk.so -lc -lm -lgcc` **without** `-static`/`-T newlib.ld`
(the default link script emits the `.dynamic`/`.dynsym`/`.rela.plt`/`.got`
sections the in-kernel loader needs), plus
`-Wl,--export-dynamic -Wl,--no-dynamic-linker -Wl,--hash-style=sysv`. This
is the path you want for a graphical app: `libembk.so` carries the entire
UI toolkit (scene graph, layout, theming, widget kit, render backend, the
DSL) compiled once, so your app links against it instead of bundling it —
apps stay well under half the size a static link would produce. See
[docs/BUILD_SETUP.md](../docs/BUILD_SETUP.md#dynamic-linking-how-libembkso-actually-works)
for how the in-kernel loader actually resolves this at process-start time
(there's no userspace `ld.so` — the kernel does it), and
[docs/EMUI_GUIDE.md](../docs/EMUI_GUIDE.md) for the app-authoring side:
`EM_APPLICATION`/`EM_WIDGET` and the component DSL built on top of all this.

`make build/hello.elf` builds the newlib demo; `make build/init.elf` builds the
freestanding pid 1; `make build/v4demo.elf` builds the fullest
dynamically-linked EmUI reference app (`user/examples/v4demo/`).

## printf format support (fixed via a newlib rebuild)

The **stock** newlib in the cross toolchain (`/usr/local/cross`, newlib 4.6.0)
was configured with `_WANT_IO_C99_FORMATS` and `_WANT_IO_LONG_LONG` **off**, so
`%z`/`%j`/`%t` and `%ll` were compiled out of its `libc.a` — `%zu` printed a
literal `"zu"`. That config is baked into `libc.a` at newlib build time and
can't be toggled from a user program.

Fixed by rebuilding newlib **with** those options into a user-owned prefix:

```
../newlib-4.6.0.../configure --target=x86_64-elf \
    --prefix=$HOME/cross/newlib-c99 \
    --disable-newlib-supplied-syscalls --disable-multilib --disable-nls \
    --enable-newlib-io-c99-formats --enable-newlib-io-long-long
make && make install
```

The Makefile's `NEWLIB_PREFIX` points the newlib compile/link at that prefix
(`-isystem …/include`, `-L …/lib` — searched before the toolchain's default,
so the fuller `libc.a` wins). `%z`, `%ll`, etc. now work. To fall back to the
stock toolchain newlib, build with `NEWLIB_PREFIX=` (empty). **Note:** the
default `NEWLIB_PREFIX` path is specific to the original author's machine —
if you're setting this up fresh, see
[docs/BUILD_SETUP.md](../docs/BUILD_SETUP.md#the-newlib-c99-rebuild-why-there-are-two-newlibs)
for the full rebuild walkthrough and how to point your own build at it.
