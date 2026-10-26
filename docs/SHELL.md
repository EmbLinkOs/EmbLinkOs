# The EmbLink Shell — Structured Pipelines, Native to the OS

**Status:** v1 shipped and live-verified (`make shell-test` = 116 host checks;
`test shell` at the kernel debug console = expression eval + a real
`ls / | where … | sort-by … | select …` pipeline over EMBKFS + the error
path, all on smp=4; `test shellio` = 26 programs that check themselves with
`assert`: text programs in pipelines, every redirection, arguments and their
limits, `#!` scripts, `source`, `which` -- `python3 tools/console_test.py
"test shellio"`).

The EmbLink shell is **not** a POSIX shell. It is a structured-pipeline
shell in the Nushell/PowerShell lineage, built on EmbLink's own primitives:
spawn-with-file-actions (no fork), typed obj-handles, and kernel pipes.
Stages exchange **typed values** (tables, records, filesizes, dates), not
byte soup — `ls | where size > 1mb` compares *sizes*, it never parses
columns out of text.

```
"ls / | where size > 100kb | sort-by size | select name size"
   │
   ▼  LEXER    (shell/lex)     bytes -> tokens; 1mb pre-parsed to bytes here
   ▼  PARSER   (shell/parse)   tokens -> AST (Pratt expressions + pipeline)
   ▼  EVALUATOR(shell/eval)    AST -> a materialized Value threaded stage->stage
   ▼  SINK     (shell/main.c)  fd 3 is a pipe? emit wire frame : pretty-print fd 1

live output:
   name        size
   ----------  --------
   libembk.so  203.4 KB
   ...
   font.ttf    741.9 KB
```

---

## 1. Layout and build

```
shell/
  value/     the Value type: null/int/float/bool/string/list/record/table
             + filesize/date/path/error. VALUE SEMANTICS: every value owns
             its data, operations clone, free is a recursive walk. No
             refcounting, deliberately (shell-scale data; zero aliasing bugs).
  wire/      Value <-> bytes. Frames: [uvarint len][TLV payload]; zigzag
             varints for signed ints, LE doubles, incremental frame reader
             that survives pipe read() boundaries. Tags are ABI: append,
             never renumber.
  sval/      the SDK any program uses (the shell, ls-like tools, the future
             terminal): structured-mode detection, emit, blocking/pump
             readers, and the human renderer (aligned table grid, "1.5 MB",
             ISO-ish dates).
  lex/       hand-written scanner. Filesize literals (1mb/512kb, 1024-based)
             lexed directly; keywords and/or/not/true/false/let; both quote
             styles ("escapes", 'raw'); # comments.
  parse/     Pratt expression parser + the pipeline/command grammar.
  eval/      scope, expression evaluator (the ONE coercion site), pipeline
             runner; eval_extern.c spawns external stages (target-only).
  builtins/  ls (OS-backed) + echo/where/select/sort-by/first/count (pure).
  hist/      history: 10,000 lines, appended to ~/.local/share/shell/history.
  edit/      the line editor (pure: bytes in, screen bytes out) and the cell
             width table it shares with the Terminal's screen model (§10).
  complete/  what Tab offers: builtins, defs, programs, $names, files.
  prompt/    the prompt's options (`prompt set`) and the prompt built from
             them: the plain one, or Powerlevel10k's (p10k*.c, pure; the
             system's answers in prompt_os.c), §10 "The prompt".
  main.c     the REPL / -c driver (shell.elf).
  test/      host tests (make shell-test) + host stubs for the OS pieces.
```

- **Includes:** kernel convention — one `-Ishell` root, qualified paths
  (`"value/value.h"`), never ui/'s per-subdir `-I` sprawl.
- **Build:** `make build/shell.elf` (static newlib, hello.elf's link shape —
  no UI dependency). Packed automatically into `embkfs.img` (which grew
  4 MB → 8 MB to fit it).
- **Test:** `make shell-test` (host: lexer/wire/parser/eval/builtins, OS
  pieces stubbed; the editor against the Terminal's real screen model,
  `shell/test/edit_test.c`; completion on a scratch directory,
  `complete_test.c`) then `test shell`, `test extern` and `test shellio` at
  the kernel serial console (live: spawn, console fds, readdir/stat, exit
  codes). `test embbuild shell` rebuilds the shell ON the OS with tcc from
  `/data/src/shell` (`shell/build.ebm`: twenty-three units, linked with tcc's
  `libtcc1.o`, which carries `__va_start`), checks it, adopts it as
  `/system/bin/shell.elf` and checks it again. A new source file goes in
  four lists: `SHELL_OBJS` (Makefile), `ARM_SHELL_SRC` (aarch64 arch.mk),
  `build.ebm`, and `_SHELL_SRC` in `tools/embkfs_mkfs/mkfs_embkfs.py`.

## 2. Running it

- **Interactive:** `run /shell.elf` at the kernel debug console (or a future
  home tile). The console delivers raw unechoed keystrokes; the shell echoes
  and handles backspace itself. `exit` leaves. Prompt: `embk>`.
- **One-shot:** `shell.elf -c "line"` — exit 0 on success, 1 on any error.
  This is what `test shell` and scripts use.
- `let` bindings persist across REPL lines (`let cutoff = 1mb`, then
  `ls | where size > $cutoff`).

## 3. The grammar

```
program  := sep* ( stmt ( sep+ stmt )* )? sep*
sep      := NEWLINE | ";"
stmt     := "let" IDENT "=" expr
          | "if" expr block ( "else" ( block | if-stmt ) )?
          | "while" expr block
          | "for" IDENT "in" expr block
          | "def" IDENT ( "(" IDENT ("," IDENT)* ")" )? block
          | "break" | "continue" | "return" expr?
          | pipeline
block    := "{" program "}"
pipeline := command ("|" command)*
command  := IDENT ( expr | redirect )*   (args end at '|', ')', '{', '}', ';', newline, EOF)
redirect := ( "out>" | "out>>" | "err>" | "err>>" | "out+err>" | "out+err>>" | "in<" ) expr
expr     := ... | "$(" pipeline ")"      -- command substitution
```

**A word that starts with `-` before a letter is a flag** -- `sh -c`,
`ls -la`, `--width=80` -- and a command receives it as its text. Before this
`-c` lexed as a unary minus applied to `c`, so no program taking a flag could
be run. `-3` is still a negative number and `a - b`, with spaces, still
subtraction.

**A NEWLINE IS A TOKEN, not whitespace.** It separates statements everywhere
except the three places a statement obviously continues: after a `|`, inside
`$( )` and `( )`, and around a block's braces. `else` may also start the line
after a closing `}`, because that is how people write it.

**Why this exists at all.** The shell could always compose commands with `|`.
It could not decide, repeat, or be extended — and with thirty builtins and
fifty-odd programs and no way to branch, a user has eighty capabilities. With
`if`, `while`, `for` and `def` they have as many as they are willing to write
down, and can add one *without rebuilding the OS*. That is the difference
between a launcher and a language.

### Command substitution is `$(...)`, never `(...)`

Plain parentheses group expressions. `(total)` would otherwise have to be a
guess between "the bare word `total`" and "run the command `total`" — and a
guess like that is wrong *silently*, in whichever direction it is made. `$` is
already how this language says "a value from somewhere", so `$(ls /)` reads as
what it is.

```
for row in $(ls /) { echo $row.name }
let n = $(ls / | count)
```

### Defined commands are functions

```
def bigger-than(n) {
    ls | where size > $n
}

bigger-than 1mb | count
```

- Arguments are **expressions, evaluated in the caller's scope**, bound to the
  parameter names in a fresh child scope whose parent is the *top-level*
  scope — not the caller's. A callee that forgot to declare something must not
  be silently satisfied by a local of whoever called it.
- Arity is checked. `one 1 2` where `one` takes one parameter is an error, not
  a silently dropped argument.
- The pipeline's input arrives as **`$in`**. A command that ignores it is a
  source; one that uses it is a filter.
- The result is the last statement's value, or whatever `return` carried — so
  a definition composes into a pipeline like any other stage.
- **A body's statements do not print.** A command is a function: its output is
  its return value and the caller decides what happens to it. The cost is that
  `def f { echo a; echo b }` yields only `b`; the benefit is that `f | count`
  counts rows instead of printing them, which is the whole point of being able
  to define one.

### Name resolution

```
builtin  >  user command  >  external program
```

Builtins win because they are the language's own verbs — `where`, `select` and
`sort-by` are as much a part of the grammar as `if` is, and a script that
shadowed one would change the meaning of every pipeline that followed it,
including ones its author never read. User commands beat programs on disk so
that `def` genuinely extends the system.

### Scripts

```
shell.elf path.esh [arg...]        run a script file
shell.elf -c "program" [arg...]    run one program from the command line
./path.esh [arg...]                a script whose first line is #!/system/bin/shell.elf
source lib.esh                     run a file HERE: its lets and defs stay
assert <condition> [message]       stop the program unless the condition holds
```

A script is read and parsed **whole**, not line by line: a block spans lines
and a line-at-a-time reader cannot see the end of one. Its arguments are the
list **`$args`**.

**`#!` is the kernel's.** A file whose first line is `#!/path/to/interpreter`
(optionally with one argument after it) runs that interpreter with the
script's path in front of the original arguments -- `./build.esh fast` is
`/system/bin/shell.elf ./build.esh fast`, `./hi.sh` with `#!/system/bin/sh`
is `sh ./hi.sh`. The interpreter is an absolute path and may not itself be a
script (one level, so scripts naming each other cannot recurse the kernel).
To this shell the line is a comment.

**`source`** runs a file's program in the current scope, so a file can be a
library of `def`s a session or another script loads. A script started by
name runs in its own shell and leaves nothing behind.

**`assert`** is how a script checks its own work: `assert $n == 3 "three
lines"` fails the program (exit status 1) with the message, and passes its
input through when the condition holds. The condition is strict, like `if`'s.

### File selection: `glob`

```
glob "*.c"                              -> a table, ls's shape
glob "src/*.c" | where size > 1kb
for f in $(glob "*.tmp") { rm $f.path }
```

A **table**, not a list of names, so every transform already written works on
the result. Rows carry `path` as well as `name` — without it, every loop over a
glob has to re-join the directory by hand and half of them get it wrong.

`*` matches within one path component and `?` matches exactly one character.
`*` deliberately does **not** cross `/`: a pattern that silently recursed would
make `rm *.tmp` a very different command from the one it looks like. Recursive
matching wants its own spelling and is not here yet. A wildcard is only allowed
in the last component.

A pattern that matches nothing yields an **empty table** — not an error, and
not the pattern passed through as a literal filename, which is the single worst
thing the Bourne shell does. `.` and `..` are never matched by a wildcard, only
by naming them.

### The vocabulary

Control flow with nothing to compute is still not much. These are all *pure* —
value in, value out, no OS — which is why they are covered by the host tests
rather than only by a boot.

| | |
|---|---|
| `sum` `avg` `min` `max` `[col]` | over a list, or a table column: `ls \| sum size` |
| `range N` / `range A B` | half-open, so `range $n` has exactly n items |
| `skip N` | `first`'s complement |
| `uniq` | distinct list items, first occurrence wins |
| `split SEP` / `join SEP` | text ↔ list |
| `lines` | text → a list of lines |
| `trim` `upper` `lower` | text maps |
| `replace OLD NEW` | every occurrence, literal |

The edge cases are the point, and each is pinned by a test:

- **An empty aggregate is `null`, not zero.** `sum` of nothing being 0 is
  defensible; `min` of nothing being 0 is a lie, and having the four agree is
  worth more than the one convenient case.
- **A null cell is skipped, not counted as zero** — a missing measurement is
  not a measurement of zero, and averaging it as one drags the answer down.
- **`split` keeps empty fields.** `"a,,b"` is three fields. A shell that
  collapsed the middle one would corrupt every CSV it touched.
- **`lines` produces no phantom last line** for the trailing newline every text
  file ends with.
- **A descending `range` is empty**, not reversed — a loop that should not run.
- **`replace` scans forward** and does not re-scan what it wrote, so
  `replace "aa" "b"` on `"aaa"` is `"ba"`.

### Background jobs, and Ctrl-C

```
wget http://host/big.iso &      -> 1        (the job id)
ls | where size > 1mb &         -> 2        (a BUILTIN pipeline, backgrounded)
jobs                            -> a table: job, state, exit, cmd
fg 1                            -> waits, and hands back { job, exit, cmd }
```

`&` backgrounds a statement by spawning **another shell** to run that
statement's source text. Spawning the named program directly would only work
for pipelines that are a single external command — builtins run *in this
process*, and this process is busy being the shell. Re-parsing the source in a
child costs a millisecond and makes backgrounding a property of any statement
rather than a privilege external programs happen to have.

`jobs` is a **table**, so it composes: `jobs | where state == "running" |
count`. A finished job is shown once and then forgotten — keeping it forever
fills the table, and dropping it the instant it exits would let a job finish
between two prompts and leave no trace it ever ran.

`fg N` waits for a job and returns its exit status. It is *not* "bring it to
the foreground" in the terminal sense: there is no terminal ownership to
transfer, because a background job here is a separate shell with its own
stdio. It is named for what it gives.

`$(cmd &)` is refused, and says why: a background job has no value yet.

**Ctrl-C interrupts a running script.** While a program runs, ^C is aimed at
the shell and *caught* rather than cancelling it; at the prompt it is handed
back and arrives as the byte the line editor treats as "kill this line".

This was impossible until the kernel grew a second channel. Cancellation is
sticky and permanent — that is the property that makes it trustworthy, since a
process cannot miss one by being between calls — so a shell that routed ^C at
itself would take one keystroke and never read a line again. The kernel now
has a **counted, clearable interrupt** alongside it (`embk_intr_catch`,
`embk_intr_take`): opt in, and ^C increments a counter you take and clear, as
many times as a human presses the key.

Which is why `while` no longer has an iteration ceiling. It used to refuse
after ten million passes, because `while true { }` could otherwise only be
ended by resetting the machine, and the comment on it said *"remove this the
day the shell can be interrupted, and not before"*. That day has come: a loop
now runs exactly as long as it is asked to, and stops when someone asks it to.

**Every command argument is an expression** — no command has a bespoke arg
parser. `where`'s arg is the expr `size > 1mb`; `ls`'s arg is the bare word
`/data`. Precedence (lowest → highest, left-assoc unless noted):

| level | operators | notes |
|------:|-----------|-------|
| 1 | `or` | |
| 2 | `and` | |
| 3 | `== != < > <= >= =~` | **non-chaining**: `a < b < c` is a parse error |
| 4 | `+ -` | |
| 5 | `* /` | |
| 6 | `not`, unary `-` | prefix |
| 7 | `.` field access, `( )` | postfix / grouping |

**Word rules (the gotchas, all deliberate):**
- `-` and `.` join a bare word only when flanked by word chars: `sort-by`
  and `file.txt` are one word; write **spaces** for infix (`size - 1`).
- `/` inside a word is a path (`/data/logs` is one word). A **standalone**
  `/` is division in infix position and the root path in prefix position
  (`ls /` works; `6 / 2` works; `6/2` is the word "6/2" — use spaces).
- `$var` never dot-joins: `$row.size` is field access on `$row`.

## 4. Settled decisions (were open in the sketch)

1. **Truthiness is STRICT.** In a boolean context (where's condition,
   and/or/not): Bool is itself, Null is false, anything else is an
   **error** naming the type. `where size` doesn't guess — it says
   "condition must be a boolean".
2. **Errors are values.** `VAL_ERROR` threads through the pipeline; the
   first one aborts the remaining stages; the driver prints `error: …` and
   keeps the prompt. No signals, no longjmp, no separate channel. In `-c`
   mode an error is exit code 1. Errors are never serialized (a frame
   carrying one is a shell bug; the wire layer rejects it).
3. **`sort-by <expr>`** (a key expression evaluated per row), not a bare
   `sort`. Ascending, stable (insertion sort — shell-scale data).
4. **Null propagation, SQL-style** (forced by real data: a directory's size
   is null): any `< > <= >=` against null is **false** — so
   `ls / | where size > 100kb` silently skips directories rather than
   aborting — while `sort-by` gives nulls a total order, **first**.
   `==`/`!=` treat null as equal only to null.
5. **Bare words fall back to strings.** An unresolved bare ident
   (EXPR_COLUMN) evaluates to its own name — `echo hi` and `ls /data` need
   no quotes; inside `where`, real columns win. `$vars` are the strict
   spelling: unresolved `$x` is an error.

## 5. Coercion (lives in eval, nowhere else)

The value layer refuses coercion (strict equality/compare); `expr_eval` is
the one visible place it happens:
- **Numeric family** INT / FILESIZE / DATE / FLOAT: mutually comparable
  (`size > 1000000` works when size is a filesize). FLOAT contaminates to
  double; otherwise int64.
- **Arithmetic keeps the richer tag:** `1mb + 512kb` is a filesize;
  `int + filesize` is a filesize; `date + int` is a date; `filesize + date`
  is an error; division by zero is an error.
- **Text family** STRING / PATH: mutually comparable bytes; `=~` is
  substring match (regex is future work).

## 6. The fd convention and the wire

- **fd 0** structured input (when it's a pipe), **fd 1** human text,
  **fd 3** structured output. *fd 3 being a pipe IS the signal* — no flag,
  no env var. A program checks with `sval_structured_out()` (fstat →
  S_ISFIFO; the newlib fstat/isatty stubs report the fd's TRUE backing:
  console = chardev/tty, pipe = FIFO/not-a-tty).
- A tool's shape:
  `if (sval_structured_out()) sval_emit(&t); else sval_print(&t, 1);` —
  and it may do BOTH (pretty table for the human on 1, frames on 3).
- **Wire format:** one value = one frame `[uvarint len][u8 tag][body]`;
  lengths/counts unsigned LEB128; INT/FILESIZE/DATE zigzag varints; FLOAT
  fixed 8B LE; strings length-prefixed bytes; records `{count,(name,val)*}`;
  tables rows of record bodies. 16 MB frame cap (hostile-length guard).
  Round-trip and byte-at-a-time reassembly are host-tested.

## 7. External programs in a pipeline

A stage whose name is not a builtin or a defined command runs a program.
A name with a `/` is a path, relative to the working directory (`./x.esh`,
`../bin/tool`); a bare name is looked for as `/system/bin/NAME.elf`,
`/system/bin/NAME`, then `/data/apps/NAME/NAME.elf`. `which NAME` says what a
name runs: `builtin`, `defined`, `structured program`, `text program`, or a
script of either kind, with its path.

**Two kinds of program.** One that links the shell SDK (`sval`) speaks the
wire, and carries a section saying so (`.embk_sval`, sval.c); the shell reads
a program file's section table -- for a `#!` script, its interpreter's --
before it plumbs the stage:

- a **structured** program gets a pipe as **fd 3** for its output (frames,
  collected: 0 → null, 1 → the value, n → a list) and, when the stage has
  input, the previous value as wire frames on **fd 0**;
- a **text** program -- sh, a port, anything that just prints -- gets the
  previous value **as text** on fd 0 (what a person would see: strings as
  they are, a list one item per line, a table as its grid) and has its
  **stdout captured as one string** when something consumes it: a later
  stage, or `$(...)`. At the **end of a statement** nothing does, and it keeps
  the terminal -- `sh`, an editor, anything interactive, works from this
  prompt, exactly as it would from any shell.

```
sh -c "printf 'a\nb\n'" | lines | count        2: sh's output, captured, split
range 4 | sh -c cat                           text in, and to the terminal
let x = $(sh -c "echo hi")                    "hi\n"
```

A text program that exits non-zero having printed nothing is an error; what
it did print is its value whatever its exit, because output a program
produced is real. A structured program that exits non-zero with no frames is
an error, as before. The captured text of one stage is kept up to 16 MB.

The shell writes input and reads output in one **streaming pump**: it feeds
fd 0 as the pipe has room and drains the output as it arrives, so a child
that emits while its stdin is still arriving cannot deadlock it.

**Arguments** are evaluated, then passed as text: strings as they are,
numbers and dates as they print, a **list as one argument per item**, a
**table as one per row** (its `path` column, else its `name` -- so
`cat $(glob "*.txt")` works). A record has no one obvious text and is an
error. The kernel's limits are checked before anything is spawned and the
error names them: at most 126 arguments, and the arguments, the environment
and their pointers together in the 4 KiB page a program's stack starts with
(`sh: the arguments (6921 bytes in 4) and the environment (29 bytes) need
7062 bytes; a program starts with 4096 for all of them`). Nothing is ever
silently shortened -- which is what an eight-slot argument array of 96-byte
strings did here before.

### Redirections

`>` compares (`where size > 1mb`), so a redirection is a **word with the
arrow attached**, written at the start of a word anywhere among a command's
arguments:

| | |
|---|---|
| `out> f` / `out>> f` | stdout to a file, replacing / appending |
| `err> f` / `err>> f` | stderr to a file |
| `out+err> f` / `out+err>> f` | both, to one file |
| `in< f` | stdin from a file |

On a **program** these are its fds 0-2 (spawn actions); a structured program
whose output is redirected gets no fd 3, so what it prints for a person is
what lands in the file. On a **builtin or a defined command**, `out>` writes
the stage's value as text and leaves null behind, `in<` makes the file's
contents the stage's input, and `err>` writes an error's message to the file
instead of failing the pipeline. A stream may be redirected once, and piped
input and `in<` together are an error.

```
sysinfo out> sysinfo.txt
sh -c "make 2>&1" out+err> build.log
range 3 out> numbers.txt;  lines in< numbers.txt | count
```

**PROVEN LIVE by two reference tools** (shell/tools/, packed on the image,
`test extern` = 4 assertions green):
- **`sysinfo.elf`** — the producer shape: emits one `{os, processes,
  kthreads, blocked, uptime_s}` record on fd 3 when piped, pretty-prints
  when not. `sysinfo | get processes` works like any builtin stage.
- **`tally.elf`** — the consumer shape: reads every frame from fd 0 to EOF,
  emits `{rows, frames}`. Deliberately an external re-implementation of
  `count`, so `ls | tally` cross-checks against `ls | count` — same data
  through the whole spawn/serialize/deserialize machinery.

Writing your own tool is exactly this pattern: include `sval/sval.h`, link
the SDK objects (see the Makefile's SHELL_SDK_OBJS rules), and branch on
`sval_structured_in/out()`. Drop the .elf on the image; the shell finds it
by name.

**Bug this proof caught (kernel, fixed):** a process's fds were released at
REAP time, so a dead external's fd-3 write end kept the output pipe's
writer count up until someone wait()ed it — but the shell drains to EOF
BEFORE waiting → deadlock on the very first external ever spawned. Fds are
now released at EXIT (thread_zombie_locked; Unix semantics: a zombie holds
no fds), with the reap-loop release kept as belt-and-suspenders. Also
caught: the PS/2 driver had NO shift layer — a pipeline shell whose
keyboard couldn't type `|`. Both fixed and re-verified.

## 8. Builtins — the standard names, native structured implementations

Every command with a familiar Unix name is OUR implementation: it emits
**typed values** (tables/records), never text to be re-parsed, and runs on
EmbLink's own syscalls. `help` prints this table live.

**Files & directories** (all paths resolve through the shell's own cwd —
`cd`/`pwd` are shell-side; the kernel stays absolute-paths-only):

| builtin | does |
|---|---|
| `ls [path]` | Table `{name, type, size, modified}` — modified is a real DATE (`where modified > ...` works); dirs get null size |
| `cat <file>` | contents as a String (pipe to `wc`, `=~`, `save`) |
| `cd [dir]` / `pwd` | change / show the working directory |
| `mkdir` / `rmdir` / `touch` / `rm` | create dir / remove EMPTY dir (SYS 57) / empty file / remove file (SYS 53/54) |
| `cp <src> <dst>` / `mv` | byte copy / copy+remove — O_TRUNC-clean overwrites |
| `save <path>` | write the piped value to a file (truncates: no stale tails) |

**Processes & system:**

| builtin | does |
|---|---|
| `ps` | Table `{pid, ppid, state, pri, kind}` (SYS_proc_list 55) |
| `kill <pid>` | terminate by pid (SYS_proc_kill 56 — the single-user ambient authority; unlike handle-scoped SYS_kill) |
| `env` | Table `{name, value}` — the environment handed to spawned commands |
| `env set K V` / `env unset K` | edit it (`setenv`/`unsetenv`; K may not contain `=`) |

**`env` is not cosmetic — it is the exact vector external commands receive.**
EmbLink inherits nothing: a child gets an environment only because a parent
passes one (kernel `spawn.h`), and `eval_extern.c` passes this shell's `environ`
explicitly, alongside the pipe file-actions. The shell itself starts with
whatever its own spawner named — usually nothing — so `env` on a fresh shell
being empty is correct, not a bug. `env set` then reaches the same `environ`,
because builtins run in-process (a `|` pipeline shares one).
| `uptime` / `date` | Record {seconds, pretty} / a DATE value from the RTC |
| `clear` | form-feed; the terminal wipes on `\f` |

**Pipeline stages (pure):**

| builtin | does |
|---|---|
| `echo expr*` | evaluates args: 0 → null, 1 → the value, n → a list |
| `where <expr>` | keeps rows where the expr is true (strict booleans, null = no) |
| `select col…` | projects columns in order; missing field → null cell (max 16) |
| `sort-by <expr>` | ascending by key, stable, nulls first |
| `first`/`head [n]`, `last`/`tail [n]` | first / last n rows (default 1) |
| `reverse` | rows in reverse order |
| `get <field>` | a table column as a List / a record's field |
| `count` | rows / items / 1 for a scalar / 0 for null |
| `wc` | text → {lines, words, bytes}; table → {rows} |
| `history` | past commands as a Table `{n, command}` (Up/Down recalls them) |
| `help` | the command reference, as a table |

Live-proven combos: `ps | where kind == "process"`,
`cd / | echo "alpha beta" | save f.txt | cat /f.txt | wc | get words | rm f.txt`
(the `test shell` batch), `cat notes.txt | wc`, `ls | sort-by size | last 3`.

## 9. Kernel surface the shell stands on (Piece 0)

| syscall | # | |
|---|---|---|
| `SYS_pipe` | 49 | returns two pipe-end **obj-handles** (not fds) |
| `SYS_handle_close` | 50 | drop a handle (generic kind dispatch) — what delivers EOF |
| `SYS_fd_install_obj` | 51 | install a pipe-end handle into the CALLER's own fd table |
| `SPAWN_ACTION_INSTALL_OBJ` | =4 | install a parent's pipe end as a child fd (COPY) |

Plus the fd layer (fds 0/1/2 are real console/pipe-backed slots; per-backing
ops with `close`/`close_locked`; exit-time fd release via the reap loop +
kworker) — all verified by `test pipe` (two-process EOF refcounting) and
`test fd`.

## 10. The line — the shell edits its own

The shell runs on a private pseudo-terminal in the Terminal app
([TERMINAL.md](TERMINAL.md)), or on the kernel console. Either way **the
line belongs to the shell**: it draws its prompt, reads every key in raw
mode, edits, completes and recalls history itself (`shell/edit`,
`shell/complete`), the way readline is bash's and zle is zsh's. The terminal
is a screen. (Until 2026-10 the Terminal edited the line and handed the
shell a finished string; the `sh` port still works that way until it moves
onto the same editor.)

### Keys

Emacs bindings, as readline and zle have them:

| Keys | Does |
|---|---|
| Left / Right, Ctrl+B / Ctrl+F | a character (an accent moves with its letter) |
| Ctrl+Left / Ctrl+Right, Alt+B / Alt+F | a word |
| Home / End, Ctrl+A / Ctrl+E | the start / end of the line |
| Backspace, Delete, Ctrl+D | delete back / forward (Ctrl+D on an empty line ends the session) |
| Ctrl+W, Alt+Backspace | kill the word before the cursor (by spaces / by word characters) |
| Alt+D, Ctrl+Delete | kill the word after the cursor |
| Ctrl+K / Ctrl+U | kill to the end / start of the line |
| Ctrl+Y, Alt+Y | yank the last kill, then cycle older kills (consecutive kills join) |
| Ctrl+_ (Ctrl+/) | undo (typing undoes a word at a time) |
| Ctrl+T, Alt+U / Alt+L / Alt+C | transpose characters; upper / lower / capitalise a word |
| Alt+. | the last word of the previous command |
| Up / Down, Ctrl+P / Ctrl+N | the previous / next line of a multi-line program, else history |
| PgUp / PgDn | history lines that begin with what is before the cursor |
| Alt+< / Alt+> | the oldest / the line being typed |
| Ctrl+R | incremental search: type to search, Ctrl+R / Ctrl+S older / newer, Enter runs the match, Escape or Ctrl+G restores the line, any other key keeps it |
| Tab, Shift+Tab | complete; again for the menu, then choose with Tab, Shift+Tab or the arrows; Enter takes the choice, Escape puts back what was typed |
| Enter | run the line -- or, if the program is unfinished, start another line |
| Alt+Enter | start another line even in a finished program |
| Ctrl+L | clear the screen, keep the line |
| Ctrl+C | abandon the line (shown with `^C`; `$status` becomes 130) |

**Unfinished programs** (`program_incomplete()` in `parse.c`): an open `{`
or `(`, a trailing `|`, an unclosed `$(`, a string that runs off the end.
Enter then adds a line under a `...` prompt instead of reporting an error, so
`if`/`for`/`def` blocks are typed the way they are written. A paste arrives
bracketed: its newlines are newlines in the program, never Enter, and
nothing pasted runs until Enter is pressed.

**Completion** offers what the shell itself would run: where a command goes
(the start of a statement, after `|`, `;`, `&`, `(` or `{`), its builtins
(`builtin_name()`), keywords, `def` commands and the programs in
`/system/bin` and `/data/apps`; after `$`, the session's names; anywhere
else, files and directories (hidden ones only for a word starting with `.`).
A name the lexer would not read as one word -- a space, a leading digit -- is
offered in double quotes; inside an open quote the quoted word completes, and
a directory leaves it open to go on into.

### History

Up to 10,000 lines (`shell/hist`), each its own allocation, so a pasted
multi-line program is one entry. In an interactive session they are appended
to `~/.local/share/shell/history` as they are accepted (backslash and newline
escaped, one entry per line) and read back when the next session starts, so
every Terminal tab adds to the same history; a file grown past 20,000 lines is
rewritten with the newest 10,000. Empty lines and repeats of the previous line
are not stored. **`history`** lists it as a Table `{n, command}`, so
`history | where command =~ "ls"` works.

### What the terminal is told (OSC 133)

Around every prompt the shell writes the standard shell-integration marks:

| Mark | When |
|---|---|
| `ESC ] 133;D;<status> BEL` | the previous command finished |
| `ESC ] 133;A;edit=shell;cwd=<dir> BEL` | the prompt starts here (again after Ctrl+L or a resize redraw) |
| `ESC ] 133;B BEL` | the prompt ends, input begins |
| `ESC ] 133;C BEL` | the command runs |
| `ESC ] 133;R BEL` | (EmbLink Terminal only) every redraw: back to the prompt's start |

`edit=shell` tells the Terminal not to edit the line itself; `cwd=` is last
and runs to the end of the mark, so a directory may contain `;`. Before the
prompt the shell resets what a program may have left on (the alternate screen,
application cursor and keypad modes, a hidden cursor, colours), sets the bar
cursor, and does zsh's PROMPT_SP: output that did not end in a newline keeps a
reverse `%` after it and the prompt starts on a row of its own.

While editing it turns on bracketed paste and **in-band resize reports**
(DECSET 2048), and turns both off before a command runs. When the window is
resized during a prompt, the Terminal erases from the prompt's first row,
resizes, and sends `CSI 48;rows;cols;height;width t`; the editor redraws from
that row at the new width, so a wrapped prompt is never left behind as a
reflowed duplicate.

### The prompt

`~/.config/shell/prompt.esh` -- what `prompt configure` writes -- and then
`~/.config/shell/rc.esh` are read when a shell starts for a person -- in the
Terminal or on the console -- and not for a script or `-c`. Each is an ordinary
program run in the session's scope, so its `let`s and `def`s stay. A mistake
in one is printed, and the shell starts anyway.

The prompt is configured with the `prompt` builtin, usually from `rc.esh`.
Options live for the session.

```
prompt                          the options, as a table {key, value}
prompt set KEY VALUE...         set one; several words are one value
prompt get KEY                  its value, or null
prompt unset KEY                forget it
prompt reset                    forget them all
prompt style [NAME]             Powerlevel10k's style NAME as the base (below);
                                plain for none; alone, the base's name
prompt import-p10k FILE         a ~/.p10k.zsh as the base -> {parameters, ignored}
prompt configure                Powerlevel10k's wizard (below)
```

A key is lower-case words joined by dots (`dir.anchor.fg`). It may be quoted,
and so may a value (`prompt set char "❯"`; `12h` is one word, though the
shell reads it as 12 and h). Without a style the prompt is the plain one,
`user@host:dir$`, and uses these:

| Option | Values | Default |
|---|---|---|
| `char` | the prompt character | `$` |
| `right` | what the right side shows, in order: `status` (✔, or ✘ and the code), `duration` (the last command's, `3s` / `1m 5s` / `1h 2m 3s`), `time` | nothing |
| `duration.threshold` | seconds a command must run for its duration to show | `3` |
| `time.format` | `24h` or `12h` | `24h` |
| `transient` | `off`, `always`, or `same-dir` (not right after the directory changed): an accepted line keeps only the prompt character, green or red by how the last command went | `off` |

The editor draws the **right side** against the edge, one column in, on the
row where the typing starts, as zsh's RPROMPT. It stays out of the way: while
the typing would come within a column of it, it is not drawn. A **transient
prompt** replaces the whole prompt once Enter accepts the line, so the
scrollback shows the commands rather than every full prompt. A prompt built
for one width is built again when the window is resized
(`edit_host.reprompt`).

#### Powerlevel10k

`prompt style lean` (or `lean-8colors`, `classic`, `rainbow`, `pure`: the
configurations `p10k configure` starts from, in `/system/share/powerlevel10k`)
or `prompt import-p10k ~/.p10k.zsh` makes the prompt Powerlevel10k's. The
engine (`shell/prompt/p10k*.c`) draws what Powerlevel10k 1.20 draws for that
configuration: its segments, separators, heads and tails, the frame, the
gap and the ruler of a prompt of several lines, colours by number, `#rrggbb`
or name, icons by `POWERLEVEL9K_MODE`, the directory shortened by every
`SHORTEN_STRATEGY` (`truncate_to_unique` against the directory listings,
anchored at folder markers such as `.git`, as far as `DIR_MAX_LENGTH` and
`DIR_MIN_COMMAND_COLUMNS` ask) and styled by its classes, the transient
prompt, and the empty line above every prompt but the first. It follows p10k's own
functions line for line, building zsh prompt strings and expanding them at
the end, so a person's configuration draws the same prompt here as on their
Mac.

Every option set is then a Powerlevel10k parameter on top of the base, named
without `POWERLEVEL9K_`, in lower case with dots for underscores, `fg` and
`bg` for `FOREGROUND` and `BACKGROUND`: `dir.anchor.fg` is
`POWERLEVEL9K_DIR_ANCHOR_FOREGROUND`, `left_prompt_elements` (or
`left.prompt.elements`) is `POWERLEVEL9K_LEFT_PROMPT_ELEMENTS`, an array
given as words. The plain prompt's five keep their meaning: `char` is the
prompt character, `right` the right side's segments (`duration` is
`command_execution_time`), `transient`, `duration.threshold` and
`time.format` p10k's own. Values are p10k's too, with its escapes
(`'%238F\u256D\u2500'`; single quotes keep the backslash).

```
# ~/.config/shell/rc.esh -- lean, as the wizard answered on one Mac
prompt style lean
prompt set mode powerline
prompt set right status duration background_jobs time
prompt set multiline.first.prompt.prefix '%238F\u256D\u2500'
prompt set multiline.last.prompt.prefix '%238F\u2570\u2500'
prompt set left.prompt.first.segment.start.symbol ' '
prompt set multiline.first.prompt.gap.fg 238
prompt set time.format 12h
prompt set transient always
```

**The segments.** `dir`, `vcs`, `prompt_char`, `status` (extended states,
`INT` for Ctrl+C's 130), `command_execution_time`, `background_jobs`, `time`,
`context` and `os_icon` are drawn, and `_joined` and `newline` work as in p10k.
Git comes from the git status daemon (below). `os_icon` shows nothing (EmbLinkOS is not an OS p10k
knows), `context` is DEFAULT or ROOT. `battery` (no battery reading reaches
the shell) and the segments for tools this system does not have (`direnv`,
`asdf`, `kubecontext`... about seventy) draw nothing, and `prompt import-p10k` lists
those a configuration names. `DIR_SHOW_WRITABLE` asks the system's
`access(2)`, which allows writing anything that exists (EMBKFS has no
permission bits).

**Git status.** `/system/bin/gitstatus.elf` (`user/gitstatus/`) is the
shell's gitstatusd: it speaks gitstatusd's protocol -- requests `id ␟ dir ␟ 0
␞` on its stdin, answers of gitstatusd's 27 fields on its stdout -- and takes
its flags (`-s -u -c -d` caps, `-m`, `-e`, `-U -W -D`, `-z`). It reads a
repository's own files: loose objects and packs (`.idx` v2, offset and ref
deltas, alternates), refs loose and packed, the configuration (the person's,
then the repository's), the index (v2 to v4, skip-worktree, assume-unchanged),
`.gitignore` files with `info/exclude` and `core.excludesFile` (git's
wildmatch), and linked worktrees and submodules. It answers the branch or
commit, upstream and push remote with the commits ahead and behind
(libgit2's walk, as gitstatusd counts), the repository's state (`merge`,
`rebase-i 1/2`, `cherry-seq`, `bisect`, `am`...), staged, unstaged, conflicted
and untracked counts, stashes, the tag at HEAD and the commit's summary. It
keeps, per repository, the HEAD tree by commit, each file's name by its stat
data, and directory listings by their mtime (EMBKFS moves a directory's mtime
when an entry comes or goes), so a prompt in a known repository reads only
what changed.

One daemon runs per shell, started on the first prompt in a repository with
the configuration's `VCS_*_MAX_NUM`, `VCS_MAX_INDEX_SIZE_DIRTY` and
`VCS_RECURSE_UNTRACKED_DIRS`; it ends when the shell does. The prompt waits
`VCS_MAX_SYNC_LATENCY_SECONDS` (0.01) for the answer, as p10k waits for
gitstatusd; later than that, it shows the repository's last known status in
the loading colours (or `VCS_LOADING_TEXT` the first time), and the editor
draws it again the moment the answer comes -- while it watches for keys -- and
a repository slower than that is not waited for next time.
`VCS_DISABLED_WORKDIR_PATTERN` leaves a repository out. On EmbLinkOS the exec
bit is not compared (EMBKFS keeps none). `~/Projects/hello` on the image is a
small sample repository to see it on; `gitclone` writes the index now too, so
a fresh clone reads clean.

`make gitstatus-test` holds the daemon to gitstatusd itself: 38 repositories
built the same way every time (fixed names and dates) in every state above,
each asked under five sets of flags, then one repository changed eleven times
under one daemon -- 201 answers, field for field
(`tools/gitstatus_golden.py`, `tests/gitstatus/golden.json`; `record` asks
gitstatusd again) -- and `gitclone`'s checkout to git (`git fsck`, `git
status`). Live, `tools/shell_gitstatus_smoke.py` reads the sample repository
on the OS: ⇡1, then !1 and ?1 as files change, nothing for an ignored one.

**The importer** reads what p10k writes: `typeset -g POWERLEVEL9K_...` with
brace lists (`{LEFT,RIGHT}_...`), `'...'`, `"..."` and `$'...'`, arrays across
lines with their comments, `+=`, locals and `${(j:|:)array}`, `unset -m`, and
`if [[ ... ]]; then ... elif ... else ... fi` on parameters already set (`==`
and `!=` with patterns, `-n`, `-z`). Other functions and commands joined by
`&&` or `||` are zsh's business and are skipped; of `my_git_formatter` the
importer keeps its colours (its `local meta='%f'` lines), and the formatter
itself is built in. A file that does not read leaves the prompt as it was and
says which line stopped it.

**Tests.** `make prompt-test` holds the engine's pieces (`tests/prompt/p10k_test.c`)
and then whole prompts to Powerlevel10k itself: `tests/prompt/golden` holds
what p10k drew under tmux on a Mac for fourteen configurations -- a person's
own `.p10k.zsh`, the four styles, and lean with each variant in
`tests/prompt/configs/variants` on top (every shortening strategy, the
directory's anchor, highlight, shortened and separator styles, classes,
`DIR_TRUNCATE_BEFORE_MARKER`, the segments' other forms) -- in sessions at 100,
60 and 36 columns (a directory to shorten among neighbours and past a folder
marker, a failure, Ctrl+C, a slow command, a background job, and repositories
as gitstatus reports them: dirty, ahead of and behind upstream with a stash and
a wip commit, a detached HEAD, a merge stopped on a conflict).
`tools/golden_prompt.py` renders the same states with `build/prompt_dump` and
compares cell for cell: character, colours, bold. `tools/record_prompts.py` records them again on a Mac with
zsh, tmux and Powerlevel10k. Live, `tools/shell_p10k_smoke.py` writes that
configuration as `prompt set` lines into `rc.esh` and checks the prompt the
shell draws on the OS, then draws every style.

#### `prompt configure`

`prompt configure` is Powerlevel10k's configuration wizard
(`shell/prompt/wizard.c`: p10k 1.20's internal/wizard.zsh, function for
function). It asks what `p10k configure` asks, in its order and on its pages,
on the alternate screen: whether the font draws a diamond, a lock, an upwards
arrow -- which decides `POWERLEVEL9K_MODE` -- then whether icons fit between
crosses, the style, character set, colours, time, separators, heads, tails,
height, connection, frame, the ornaments' colour, spacing, icons, flow and the
transient prompt, each page showing the prompts to choose between. A page
that does not fit the window drops its blank lines, then its diagrams, then
its sample prompts, as p10k's does, and a window under 47 columns by 14 lines
gets p10k's "Not enough space" page until it is resized. `r` starts over; `q`
or Ctrl+C quits.

Two questions are not asked. Instant prompt is how zsh shows a prompt before
.zshrc has finished -- this shell has no slow startup to hide -- so the
configuration keeps the template's `INSTANT_PROMPT` and the header does not
list it. p10k's last question, whether to change .zshrc, has nothing to
change here: the shell reads prompt.esh itself.

The answers make the configuration `p10k configure` would make: the style's
template from /system/share/powerlevel10k with the same lines changed, byte for
byte but two words of the header (it says to type `prompt configure`, and it
does not list instant prompt). It goes to `~/.config/shell/p10k.zsh` -- an
older one is kept beside it as `p10k.zsh.<date>-<time>` when the person says to
overwrite it -- and `~/.config/shell/prompt.esh` loads it in every shell from
then on, under the same "Wizard options:" header:

```
let p10k = $(prompt import-p10k "~/.config/shell/p10k.zsh")
```

(`$p10k.ignored` lists the segments it names that draw nothing here). It is
read before rc.esh, so a person's own `prompt set` lines there still win. The
shell that ran the wizard loads the configuration straight away.

**The first start.** A shell in the Terminal that finds no prompt
configuration -- neither file loads a style or sets an option -- runs the
wizard before its first prompt, as zsh with Powerlevel10k does, and p10k's
greeting says why. Quitting it says how to keep it away (`prompt set
disable.configuration.wizard true` in rc.esh, an option that does nothing
else); otherwise it runs again in the next shell. It does not start by itself
in a Live session -- a disposable one for trying the system, which skips the
first-boot setup the same way -- or when EMBLINK_NO_WIZARD is set; `prompt
configure` runs it anywhere there is a terminal of that size.

**Tests.** `make prompt-test` holds the wizard to `p10k configure` itself:
`tests/prompt/wizard` holds fifteen paths of answers recorded under tmux on a
Mac (`tools/record_wizard.py`) -- between them every answer to every question,
a restart, an overwrite, quitting, the wizard a shell with no configuration
starts, and a window resized too small and back -- at 100x40, 80x24 and 47x14.
`tools/golden_wizard.py` drives the wizard (`build/wizard_dump`) with the same
keys and compares 454 pages cell for cell, and 13 configurations line for
line. The recording runs where EmbLinkOS does as far as the wizard can tell:
256 colours and 24-bit ones (COLORTERM), an OS p10k has no icon for, the
configuration at ~/.config/shell/p10k.zsh. The answers in the header of the
Mac's own ~/.p10k.zsh (`tests/prompt/configs/user.p10k.zsh`) make that file
again, but its date. Live, `tools/shell_wizard_smoke.py` makes an account on a
fresh image, logs in and opens the Terminal: the wizard starts by itself, the
same answers are typed page by page, and the prompt, the files, a new tab and
`prompt configure` are checked.

### `$status`

`$status` is the last line's result: 0 on success, 1 on an error, 130 for a
line abandoned with Ctrl+C -- typed and not run, or stopped while it ran (a
program it started, a loop, a `sleep`). The same number is the `D` mark's, and
the prompt's: Powerlevel10k shows 130 as `INT`.

### The console

On the kernel console, which parses no escape sequences, the same editor
redraws by backspacing and rewriting (`EDIT_DUMB`): the keys arrive as the
keyboard driver's private bytes (Left 0x11, Right 0x12, Up 0x13, Down 0x14,
Home 0x02, End 0x05, Delete 0x7F, PgUp/PgDn 0x0E/0x0F) -- the console cannot
tell them from Ctrl+Q/R/S/T/B/E, which the Terminal can. A multi-line program
is edited one line at a time there, and completion lists its matches instead
of opening a menu.

## 11. The small kernel batch (shipped)

Three gaps closed in one pass, all `test fd`-asserted live:
- **`O_TRUNC` is live** — open() shrinks to 0 through the per-fs `truncate`
  op (EMBKFS: `embkfs_truncate_object`, a real COW transaction). Both open
  paths (`vfs_open` + `fd_open_into`) enforce it; a fs without the op fails
  `-ENOSYS` loudly. `save`/`cp` overwrite cleanly now — the stale-tail trap
  is closed.
- **`rmdir`** — SYS 57 over the new per-fs `rmdir` op (EMBKFS's
  `embkfs_rmdir_name` enforces emptiness; never recursive). The shell's
  `rmdir` builtin; `mkdir x | rmdir x` is in the `test shell` batch.
- **`mtime` in `vfs_stat`** — EMBKFS's ns-resolution inode timestamps
  surfaced as seconds; syscall layer zeroes the struct so FAT32/epfs report
  an honest 0. Mirrored through `embk_vfs_stat`/`embk_stat`/newlib
  `st_mtime` (ABI: all three grow together, apps rebuilt). `ls` gained a
  `modified` DATE column.

## 12. What's deliberately NOT here (roadmap)

- **Streaming stages** (`tail -f` shapes) — v1 materializes.
- **Regex `=~`** — substring today.
- **Stopping and resuming a job** (`^Z`, `bg`). `&`, `jobs`, `fg` and ^C into a
  running script all work; what is missing is suspending something already
  running and putting it back. That needs the kernel to be able to *stop* a
  process rather than only interrupt or cancel it.
- **Interrupting a background job.** ^C reaches the foreground only, and
  `kill` takes a pid rather than a job id.
- **Closures / commands as values.** `def` creates a name, not a value; a
  command cannot yet be passed to another command.
- **Early `return` from a script**, as distinct from from a command body —
  today a top-level `return` sets the exit status and stops that block only.
- **cwd / relative paths, env vars, Ctrl-C** — tracked in the libc/OS
  roadmap, not shell-local.
- ~~**dup2-style fd aliasing / redirection syntax**~~ — done: `out>`, `err>`,
  `out+err>`, `in<` and the appending forms (§7, Redirections). POSIX spellings
  (`2>&1`, `<<EOF`) are sh's; here-documents are not in this shell.
- **Text stages stream into memory.** A text program's output is captured
  whole (to 16 MB) before the next stage starts, like every stage here;
  `tail -f` shapes need streaming stages.
