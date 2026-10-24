# Spawn-based sh

EmbLinkOS ships an initial `sh` port based on mrsh, alongside the native
structured shell. Start it with `sh` in Terminal, run a script with
`sh file.sh`, or use `sh -c 'echo "$HOME"'`. `exit` returns to the native
prompt. The image installs both `/system/bin/sh.elf` and `/system/bin/sh`.
This is a bounded development shell, not complete POSIX or zsh compatibility.

## Working language and utilities

The tested language includes quoting, variables, positional parameters,
parameter expansion, integer arithmetic, functions, conditionals, loops,
case statements, aliases, sourced files, subshells, command substitution,
pipelines, background commands and `wait`. File redirection, append,
descriptor duplication/closure and quoted/unquoted here-documents work.
External commands receive exported variables and command-local assignments.
Globbing uses private BSD/newlib pathname matching in the C locale.

The port supplies `echo`, bounded integer/string `printf`, `test`/`[`, `cat`,
`sleep` and `help`, in addition to mrsh's shell builtins. `printf` supports
`s`, `c`, `b`, `d`, `i`, `u`, `o`, `x`, `X`, bounded width/precision and
escapes; floating-point formats, stars and length modifiers are unsupported.
Supported option flags are `-a`, `-f`, `-n`, `-u` and `-x`. Other options,
including `-e`, `-C` and `-m`, are rejected. This is not yet the utility set
needed for arbitrary configure scripts or foreign build systems.

For example:

```sh
for board in sensor display power; do
    printf 'Checking %s\n' "$board"
done
result=$(printf '%s' ready)
printf '%s\n' "$result" > result.txt
cat result.txt | cat
```

## Execution without fork

External programs use the existing native `posix_spawn` adapter. Shell code
that needs isolation runs in another instance of `sh` with a typed AST and
explicit state snapshot. The snapshot preserves variables and attributes,
functions, aliases, positional arguments, supported options, last status and
shell identity. It is not reconstructed from a display-formatted command.
The worker reads its context from an immediately unlinked temporary descriptor
and closes that descriptor before executing code. Descriptors are transferred
explicitly, and unused pipe ends and close-on-exec descriptors are excluded.

Context and AST transport are limited to 1 MiB, 16,384 nodes and 96 levels.
Pipelines support 16 stages; up to 32 uncollected background results are cached.
Workers require a writable `HOME` or `TMPDIR` (then `.` as a fallback) and
filesystem capability. Temporary file data still incurs filesystem work;
an anonymous memory transport is future work. Native startup limits still
apply to external argv/environments: 31 environment entries, 1024 environment
bytes, 64 descriptors and 61 user spawn actions. See the
[process contract](PROCESS_COMPATIBILITY.md) for the complete adapter limits.

Terminal uses OSC prompt markers and its existing local history/caret editing.
Canonical echo is respected, so submitted commands appear once. Foreground
interrupts use native owned-child cancellation with a 500 ms hard-kill
backstop. The shell restores its prompt afterward and scopes cancellation to
foreground children. This is native interruption, not POSIX signals or job
control.

## Deliberate limits and next work

- No `fork`, in-place command `exec`, process groups, stopped jobs, general
  signal delivery or resize signals. Command `exec` returns 126; `exec`
  without a command remains a no-op.
- Run text scripts through `sh file`; kernel shebang loading and automatic
  executable-text fallback are not implemented.
- Temporary assignment prefixes on builtins/functions need further work;
  the tested command-local environment path is for external programs.
- Workers do not transport original AST source positions, so diagnostic
  locations and `$LINENO` do not have full fidelity there.
- Command substitution currently accumulates output in memory without a
  separate output limit. The transport bounds do not bound produced output.
- Arithmetic retains upstream limitations; comprehensive overflow and recursive
  expression hardening remain open.
- Socket inheritance, shared descriptor status flags and full append semantics
  retain the adapter limitations documented in the process contract.
- No broad Unix utilities, SSH, Vim, zsh or CPython subprocess compatibility is
  implied. Those need their own implementation and acceptance.

## Build and verification

The source closure is in `ports/mrsh/sources.mk`; image builds include it by
default. The AArch64 rules compile the port separately without applying its
private headers or macros to unrelated programs. Runtime acceptance here is
x86_64 UEFI/NVMe QEMU; AArch64 compile/link is a separate check.

```sh
make posixsh-test posixsh-sanitize-test
# In the booted serial debug console:
test posix-shell
# With the cloud toolchain activated and fresh images:
python3 tools/posixsh_smoke.py
```

The shared corpus contains 83 shell cases; 79 are also compared against
`/bin/sh`. The other four cover explicit unsupported behavior and the native
FD 63 extension. Host ASan/UBSan runs enable leak detection for the shell and
workers. Separate tests exercise 80 worker startup boundaries, 5759 typed AST
checks and 20,992 deterministic codec mutations. These are selected regression
tests, not the complete upstream suite or a POSIX certification.

The native C witness runs the same cases through real child processes and
checks output, status and descriptor stability. The desktop harness repeats
it, exercises interactive loops/arithmetic/substitution, drains a 66,049-byte
three-stage pipeline, interrupts a program that declines cancellation while a
background job survives, waits for that job, and returns to the native shell.

On the delivered x86 image the witness passes **338/338 checks** in each of
two serial-wrapper runs and again inside the logged-in Terminal namespace.
The serial regressions also pass 357 spawn, 203 process and 108 PTY checks,
with pipe/device allocation returning to baseline. The capability suite checks
that native and POSIX spawn OPEN actions cannot bypass filesystem permission.
The wider device/preferences/files/document/page-cache/IPC/compositor/session
and kill-I/O integration batch passes. Source-attributed original screenshots
are in the [progress gallery](media/laptop-progress/README.md).

## Provenance

`third_party/mrsh/UPSTREAM.json` pins the upstream revision and original source
hashes. The pristine import and subsequent adaptations are separate commits.
mrsh's MIT notice and the BSD pathname helper notices are shipped under
`/system/licenses`. See `third_party/mrsh/EMBLINK.md` and
`third_party/newlib-posix/README.md` for source details.
