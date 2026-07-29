# Building for (and on) EmbLinkOS

How software is built for the OS. This is the SDK-facing overview; the full
compiler/linker CLI reference lives with the tool it documents, in the EmbCC repo
at `EmbCC/docs/USAGE.md`.

## The toolchain

EmbLinkOS is built by **EmbCC** (our own C compiler) and **EmbLD** (our own
linker) — the pair that compiled and linked the *kernel* to a booting desktop with
no GCC or `ld`. For ports that predate self-host, the cross toolchain
(`x86_64-elf-gcc` + our rebuilt newlib) is still used; both target the same ABI.

- `embcc -c file.c -o file.o` — compile a C translation unit to an ELF object.
- `embld -o out.elf … .o` — link objects into an ELF executable.
- `embld --embx --cap NAME … -o out.embx … .o` — link into an **EMBX** binary that
  *declares the capabilities it needs* (see below).

Full flags (`-E`, `-I`, `-g`, `-O0/1/2`, the `-mno-sse`/`-mno-red-zone` family,
`--cap`, `-Ttext`, `-e`): **`EmbCC/docs/USAGE.md`**.

## The ABI — what "targeting EmbLinkOS" means

An app is defined by the sealed ABI under **`/system/abi`** (read-only,
`docs/USERSPACE.md` D2):

- `/system/abi/crt0.o` — `_start` (stack alignment, `environ`, calls `main`).
- `/system/abi/syscalls.o` — the newlib retargeting layer (POSIX → our syscalls).
- `/system/abi/libc.a` — the C library.
- `/system/abi/include/embk.h`, `embk_syscall.h` — the syscall/SDK surface.
- `/system/lib/libembk.so` — the shared UI/runtime toolkit (EmUI apps link it).

Compile against those headers, link against `crt0.o` + `syscalls.o` + `libc.a`
(the freestanding path — `init.elf`/`primtest.elf` — instead brings its own
`_start` and no libc; see `user/lib/user.ld`).

```sh
embcc -c app.c -I/system/abi/include -o app.o
embld -o app.elf /system/abi/crt0.o app.o /system/abi/syscalls.o /system/abi/libc.a
```

## Declaring authority (the part that's ours)

An EmbLinkOS app declares the authority it needs, and the kernel enforces it — see
[USERSPACE_v2.md](USERSPACE_v2.md):

- **Capabilities** — resource classes (`filesystem`, `network`, `gpu`, …). An EMBX
  binary carries its declared cap set (`embld --embx --cap filesystem`); the kernel
  checks it is a subset of the grantor's at load. (ELF apps inherit the spawner's
  caps.)
- **Namespace** — the paths the app can name, declared in a per-app manifest
  `user/bin/<name>.ns` (lines of `<ro|rw> <prefix>`), packed to
  `/data/apps/<name>/<name>.ns`. The session grants exactly that (UP4). No manifest
  ⇒ the app inherits the parent's view.

Where this is heading — one build manifest generating the EMBX cap table + the
`.ns` + a package manifest, and `pkg install` granting exactly the declared
authority — is designed in [PACKAGING_AND_SDK.md](PACKAGING_AND_SDK.md).

## Cross-built vs on-OS

- **Cross-built (today's default):** drop `user/bin/foo.c` in and `make` compiles it
  to `build/foo.elf`; mkfs packs it to `/data/apps/foo/` (plus its `.ns` if present).
  See [BUILD.md](BUILD.md).
- **On the OS itself:** `tcc` and `embcc` run *on the image* — you can compile and
  link an app on the machine (`/data/apps/tcc/tcc.elf`, and the staged `embcc`).
  EmbBuild (`build.ebm` manifests) drives multi-unit on-OS builds; the shell and the
  kernel have both been rebuilt on-device. The remaining step to a fully self-owned
  build (an on-OS assembler for the `.asm`) is tracked in [BUILD.md](BUILD.md) §12
  and `EmbCC/docs/todo.md` (A1).

## See also

- `EmbCC/docs/USAGE.md` — the compiler/linker CLI reference.
- [BUILD.md](BUILD.md) — the OS image build + the EmbBuild-builds-the-kernel plan.
- [USERSPACE_v2.md](USERSPACE_v2.md) — capabilities + namespaces (what apps declare).
- [PACKAGING_AND_SDK.md](PACKAGING_AND_SDK.md) — packaging + the SDK direction.
