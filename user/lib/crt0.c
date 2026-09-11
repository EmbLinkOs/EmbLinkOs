/* user/crt0.c — EmbLink OS's newlib crt0. No _start.S, no stack-popping:
 * process_trampoline already delivers argc/argv in rdi/rsi, exactly where a
 * normal C function expects its first two parameters. The only asm needed
 * is the three-instruction _start stub below -- not for argument delivery
 * (rdi/rsi pass through untouched), but because a raw ELF entry point and
 * an ordinary CALLed C function follow DIFFERENT stack-alignment
 * conventions that a plain C function can't reconcile for itself (see
 * _start's comment below).
 *
 * Deliberately missing, not overlooked: no envp (nothing builds an
 * environment yet -- passed as NULL), no __libc_init_array() call (skips
 * C++ static constructors / GCC constructor attributes -- fine for a
 * pure-C toolchain; revisit only if this ever needs to build C++). */

/* The one include: crt0 otherwise declares its externs by hand (no libc headers
 * this early), but setup_tls() has to reach the kernel to install %fs and
 * hand-rolling the syscall stub would just duplicate this header. It is
 * freestanding -- inline asm and constants only. */
#include "embk_syscall.h"

extern int main(int argc, char **argv, char **envp);
extern void exit(int code);   /* newlib's real exit() -- NOT user/init.c's
                                * hand-rolled one. This one runs atexit()
                                * handlers and flushes stdio before calling
                                * newlib's _exit(), which is OUR syscalls.c
                                * stub (the one wrapping SYS_exit). */

/* The REAL entry point: three instructions, not a C function, on purpose.
 * process_trampoline() delivers control via a raw `iretq` (no `call`), with
 * RSP set to a 16-byte-aligned child_user_rsp (process.c's `off &= ~0xFULL`)
 * -- correctly following the x86-64 SysV ABI's PROCESS-entry convention
 * (RSP === 0 mod 16, no synthetic return address on the stack). A plain C
 * function compiled as `_start` can't know that: GCC always assumes the
 * ORDINARY called-function convention instead (RSP === 8 mod 16, as if a
 * `call` just pushed an 8-byte return address) and arranges its own
 * internal `call`s to land on 0-mod-16 relative to THAT assumed baseline.
 * Since the real baseline here is already 0-mod-16, a plain-C `_start`'s
 * `call main` would actually execute misaligned by 8 bytes. Silent today
 * only because this whole toolchain builds with -mno-sse/-mno-sse2 (no
 * aligned SIMD load/store ever gets emitted to fault on it) -- would become
 * a real, hard-to-diagnose crash the moment real newlib code (compiled
 * normally, using SSE-optimized memcpy/memset/etc.) gets linked in.
 *
 * `and $-16, %rsp` re-establishes 16-byte alignment (a no-op today given
 * child_user_rsp is already aligned, but this is the same defensive
 * realignment every real crt0 does, and it's what a raw entry point is
 * responsible for providing for itself). `call start_c` -- not `jmp` --
 * is what then correctly turns that 0-mod-16 baseline into the 8-mod-16
 * a normally-compiled C function expects at ITS OWN entry (the `call`'s
 * pushed return address IS that missing 8 bytes). Neither instruction
 * touches rdi/rsi/rdx, so argc/argv/envp arrive at start_c() exactly as
 * process_trampoline left them. start_c()/exit() never return in practice;
 * the trailing hang loop is only a defensive backstop. */
#if defined(__x86_64__)
__asm__(
    ".global _start\n"
    "_start:\n"
    "    and $-16, %rsp\n"
    "    call start_c\n"
    "1:  jmp 1b\n"
);
#elif defined(__aarch64__)
/* The aarch64 entry stub -- ARM64.md phase A6.
 *
 * The alignment argument above is an X86 argument, and it does not carry over:
 * aarch64's PCS requires SP to be 16-byte aligned at ALL times, not 8-mod-16
 * inside a called function, because there is no return address on the stack to
 * account for -- `bl` puts it in x30. So `bl` versus `b` changes nothing about
 * alignment here, and start_c() sees the same SP whichever is used. The `and`
 * is kept for the same defensive reason x86 keeps it: a raw ELF entry point is
 * responsible for the alignment it hands on, and SCTLR_EL1.SA makes a
 * misaligned SP a fault rather than a slow path.
 *
 * x29/x30 are zeroed to TERMINATE THE FRAME CHAIN. A debugger (or the kernel's
 * own backtrace) walks frame pointers until it sees zero; entering with
 * whatever the eret left there makes the first backtrace of any crashing
 * program run off into nonsense. x86 gets this free -- process.c's iretq frame
 * has no rbp to inherit -- and aarch64 has to say it.
 *
 * x0/x1/x2 (argc, argv, envp) are untouched: x9 is a scratch register in the
 * PCS and deliberately not one of the argument registers. */
__asm__(
    ".global _start\n"
    "_start:\n"
    "    mov  x29, #0\n"
    "    mov  x30, #0\n"
    "    mov  x9, sp\n"
    "    and  sp, x9, #-16\n"
    "    bl   start_c\n"
    "1:  b    1b\n"
);
#else
#error "crt0.c: no entry-point stub for this architecture"
#endif

/* ---- the crt-stuff a -nostartfiles link doesn't get --------------------
 * `__dso_handle` normally comes from crtbegin.o. We link -nostartfiles (crt0
 * provides _start), so nothing defines it -- and the FIRST C++ program with a
 * global destructor fails to link: g++ emits
 * `__cxa_atexit(dtor, obj, &__dso_handle)`, where the handle identifies WHICH
 * module owns the destructor so a dlclose() could run just that module's.
 *
 * Its own address is the conventional value: it only has to be a unique,
 * stable token per module, never dereferenced. There is exactly one module
 * here, so uniqueness is free -- but define it we must, or C++ objects with
 * static-storage destructors simply cannot link. */
void *__dso_handle = &__dso_handle;

/* ---- static initializers (.init_array) ---------------------------------
 * Emitted by the linker script (newlib.ld) with these bracket symbols, but
 * NOTHING RAN THEM until now -- the array was laid down and ignored.
 *
 * Each entry is a function the compiler wants called BEFORE main:
 *   - C++ global/static constructors (`static Foo g_foo;`)
 *   - GCC's __attribute__((constructor)) in plain C
 * Skipping them is why crt0 was "pure-C only": a C++ program would enter
 * main with every global still raw zeroes, then crash or silently misbehave
 * the moment it touched one. This is the crt0 half of C++ support.
 *
 * Hand-rolled rather than newlib's __libc_init_array() because that also
 * runs _init/.init (the legacy crtbegin/crtend path) which this bare
 * -nostartfiles link has no crtX objects for. Walking the array IS the
 * modern contract; order is the linker's (SORT'ed by priority).
 *
 * TWO schemes are walked, because GCC chooses between them at ITS OWN
 * configure time (--enable-initfini-array): the stock bare-metal C cross
 * compiler (built --without-headers) emits the LEGACY .ctors, while a
 * newlib-aware/C++ build emits the modern .init_array. Walking only one
 * silently does nothing for objects built by the other -- exactly the trap
 * this hit first time round: the constructor sat unrun in an uncollected
 * .ctors while the .init_array brackets came out empty (start == end). Run
 * both; whichever is empty costs a single compare.
 *
 * .fini_array (destructors / atexit-at-image-scope) is deliberately NOT run
 * here: exit() is what owns that, and nothing needs it yet. Named, not
 * forgotten. */
/* WEAK on purpose: this ONE crt0.o is linked two different ways. Static
 * programs (shell/sysinfo/tally/hello) use `-T user/lib/newlib.ld`, which
 * defines all four brackets. The dynamically-linked EmUI apps
 * (NEWLIB_DYN_LDFLAGS -- no -T, ld's DEFAULT script) get
 * __init_array_start/end from that script but NOT __ctors_start/end, so a
 * strong reference is an instant "undefined reference to __ctors_end" and
 * every EmUI app stops linking. Weak means an absent bracket resolves to 0
 * and the walk is simply skipped -- crt0 stays correct under any linker
 * script, present or future. */
extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));
extern void (*__ctors_start[])(void) __attribute__((weak));
extern void (*__ctors_end[])(void) __attribute__((weak));

/* ------------------------------------------------------------------ */
/* Thread-local storage                                                */
/* ------------------------------------------------------------------ */

/* Geometry of the TLS template, handed over by user/lib/newlib.ld. These are
 * ABSOLUTE linker symbols: their VALUE is the number, so we take the ADDRESS
 * and cast -- reading them as variables would dereference address 0x10-ish and
 * fault.
 *
 * WEAK for the same reason as the constructor brackets above, and it is not
 * hypothetical: crt0.o is linked TWO ways. Static programs use -T newlib.ld,
 * which defines these; the dynamic EmUI apps use NEWLIB_DYN_LDFLAGS (no -T,
 * ld's DEFAULT script), which does NOT. A strong reference would break every
 * EmUI app's link instantly. Weak ⇒ absent ⇒ 0 ⇒ memsz == 0 ⇒ the setup
 * below does nothing, which is exactly right for a program with no TLS.
 * KNOWN GAP: that also means dynamically-linked EmUI apps get no TLS at all --
 * fine while nothing there uses __thread, but a `__thread` variable in one
 * would fault on %fs. Fix by teaching the dynamic link the same symbols. */
/* Both are ADDRESSES -- __tls_geom points at three quadwords (filesz, memsz,
 * align) the linker script emitted as DATA. The geometry used to be three
 * linker symbols holding the numbers themselves, and that does not survive a
 * position-independent link: PIC code reaches a symbol PC-relatively, so the
 * program computes load_bias + value, which is right for an address and
 * nonsense for a size. Reading them out of memory means there is nothing to
 * bias. newlib-body.ld's comment carries the full reasoning. */
extern char __tls_image[] __attribute__((weak));
extern char __tls_geom[]  __attribute__((weak));   /* 3 quadwords: filesz, memsz, align */

extern void *malloc(unsigned long size);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);

/* Room reserved at the thread pointer for the Thread Control Block.
 *
 * x86-64 (variant II): only the self-pointer at offset 0 is architecturally
 * required (`mov %fs:0x0,%reg` is how every TLS access starts), but the psABI's
 * TCB conventionally holds more -- notably a DTV pointer at +8 and the
 * stack-protector canary at +0x28. We reserve and ZERO 64 bytes so anything
 * that pokes at those reads a defined zero rather than heap garbage. The TCB is
 * ABOVE the thread pointer there, so its size is ours to choose.
 *
 * aarch64 (variant I): 16, and NOT a free choice -- see setup_tls(). The TCB
 * sits BELOW the block, so its size is part of every variable's address, and
 * the linker has already committed to 16. Reserving "a bit extra for safety"
 * would move every TLS variable and silently corrupt them. */
#if defined(__x86_64__)
#define TCB_SIZE 64
#elif defined(__aarch64__)
#define TCB_SIZE 16
#endif

/* Build this thread's static TLS block and point the thread pointer at it.
 *
 * THE TWO ARCHITECTURES DISAGREE HERE, and this is the one place in the
 * userland where they genuinely do -- docs/TODO.md has been saying so since
 * A5. Both are "static TLS, one module, no dynamic loading", but the ELF TLS
 * variants put the block on opposite sides of the thread pointer:
 *
 *   x86-64, VARIANT II            aarch64, VARIANT I
 *   [ .tdata | .tbss ][ TCB ]     [ TCB ][ .tdata | .tbss ]
 *                     ^TP         ^TP
 *   addr = TP - align(memsz)+o    addr = TP + align_up(16, a) + o
 *
 * Get the arithmetic wrong and there is no fault to debug: every TLS variable
 * quietly resolves to the wrong address. Both formulas are the LINKER's, so
 * both must use the linker's own alignment (__tls_geom[2]) rather than a
 * guess.
 *
 * On aarch64 the 16 is not a constant we picked -- it is TCB_SIZE in binutils'
 * elfNN_aarch64_tpoff_base(), which computes every TPREL offset as
 * align_power(16, tls_align) + the variable's offset in the segment. It is why
 * TCB_SIZE above is 16 there and must stay 16.
 *
 * Called before the constructors: a C++ ctor may touch a __thread variable.
 * malloc() is safe this early -- newlib is built --enable-threads=single, so its
 * allocator uses _impure_ptr, not TLS; it would be circular otherwise. */
static void setup_tls(void)
{
    /* Absent (a dynamically-linked EmUI app, or the stubs in
     * emlink_dynstubs.s) => __tls_geom is a null pointer => the program has no
     * TLS and we do nothing, which is the same answer the old absolute symbols
     * gave for memsz == 0. Checked BEFORE the load, not after: dereferencing it
     * is the one thing we must not do. */
    if (!__tls_geom) {
        return;
    }
    const unsigned long *geom = (const unsigned long *)__tls_geom;
    unsigned long filesz = geom[0];
    unsigned long memsz  = geom[1];
    unsigned long align  = geom[2];

    if (memsz == 0) {
        return;             /* no PT_TLS: nothing to set up, the thread pointer
                             * stays unused */
    }
    if (align == 0) {
        align = 8;
    }

    unsigned long tls_size = (memsz + align - 1) & ~(align - 1);

    /* + align of slack so TP can be rounded to the linker's alignment while
     * the whole block still fits inside the allocation. */
    char *base = (char *)malloc(tls_size + TCB_SIZE + align);
    if (!base) {
        return;             /* nothing sane to do this early; a TLS read will
                             * fault loudly rather than read someone else's data */
    }

    unsigned long tp;
    char *block;

#if defined(__x86_64__)
    /* TP is the TOP of the block; the data grows down from it. */
    tp    = ((unsigned long)base + tls_size + align - 1) & ~(align - 1);
    block = (char *)(tp - tls_size);

    memset((void *)tp, 0, TCB_SIZE);
    *(unsigned long *)tp = tp;   /* the self-pointer `mov %fs:0x0,%reg` reads */
#elif defined(__aarch64__)
    /* TP is the BOTTOM: the TCB is at TP, the data starts after it, rounded up
     * to the segment's alignment exactly as the linker rounded it. Aligning TP
     * itself to `align` is what makes that sum aligned too. */
    unsigned long tcb_off = (TCB_SIZE + align - 1) & ~(align - 1);

    tp    = ((unsigned long)base + align - 1) & ~(align - 1);
    block = (char *)(tp + tcb_off);

    /* Zero the whole TCB. Nothing here writes a self-pointer: variant I has no
     * such requirement -- a TLS access is `mrs x, tpidr_el0` plus an immediate,
     * with no indirection through the TCB at all. The first word is where a
     * dynamic loader would keep the DTV, and zero is the honest value for a
     * program that has no dynamic TLS. */
    memset((void *)tp, 0, tcb_off);
#endif

    if (filesz) {
        memcpy(block, __tls_image, filesz);        /* the .tdata initialisers */
    }
    memset(block + filesz, 0, tls_size - filesz);  /* .tbss + alignment tail */

    /* The kernel is the thread pointer's owner on BOTH architectures, and for
     * different reasons. On x86 it has no choice: CR4.FSGSBASE is off, so
     * WRFSBASE would #UD here. On aarch64 EL0 CAN write TPIDR_EL0 directly --
     * but it must not, because process.c reinstalls thread::fs_base on every
     * context switch, so a base this process set behind the kernel's back would
     * survive exactly until the next preemption. One syscall, one owner, one
     * value that stays true. Ignore the return -- if it fails the first TLS
     * access faults, which is a better signal than anything we could print. */
    embk_syscall1(EMBK_SYS_set_fs_base, (int64_t)tp);
}

static void run_init_array(void)
{
    if (!__init_array_start || !__init_array_end) {
        return;                 /* this link has no such section */
    }
    /* modern: forward, linker-sorted by priority */
    for (void (**fn)(void) = __init_array_start; fn != __init_array_end; fn++) {
        if (*fn) {
            (*fn)();
        }
    }
}

static void run_ctors(void)
{
    if (!__ctors_start || !__ctors_end) {
        return;                 /* e.g. the default script: no brackets */
    }
    /* legacy .ctors: BACKWARD -- that is this format's contract (crtbegin's
     * __do_global_ctors_aux walks from the end down). No crtbegin/crtend
     * means no -1/0 sentinels, but skip them defensively in case a linked-in
     * object ever brings its own. */
    for (void (**fn)(void) = __ctors_end; fn-- != __ctors_start; ) {
        if (*fn && *fn != (void (*)(void))-1) {
            (*fn)();
        }
    }
}

/* Defined in syscalls.c. `environ` is what getenv()/setenv() walk; the kernel
 * hands us the vector in RDX and this is where the two meet. */
extern char **environ;
extern char *embk_empty_env[1];

/* syscalls.c: seeds the working directory from PWD, if the parent named one.
 * Must run AFTER environ is installed and BEFORE anything resolves a relative
 * path -- a ctor that opens a data file is exactly that. */
extern void embk_cwd_init_from_env(void);

__attribute__((noinline, used))
static void start_c(int argc, char **argv, char **envp)
{
    /* Publish the environment BEFORE ctors and main: a C++ static initialiser
     * or an __init_array entry may legitimately call getenv().
     *
     * envp==NULL means the parent gave us NO environment (the EmbLink default --
     * nothing is inherited unless named; see kernel spawn.h). Point `environ` at
     * an empty vector rather than leaving it NULL: getenv() must be able to walk
     * it and answer "unset", and every caller expects environ to be
     * dereferenceable. "No environment" and "an environment with nothing in it"
     * are indistinguishable to a reader, and only one of them is safe. */
    environ = envp ? envp : embk_empty_env;

    /* The working directory a parent HANDED us (PWD), or "/" if it named none.
     * Nothing is inherited on EmbLink -- see syscalls.c's g_cwd. Ordered after
     * environ (it reads it) and before ctors (one may open a relative path). */
    embk_cwd_init_from_env();

    setup_tls();                            /* %fs BEFORE any ctor: one may
                                             * touch a __thread variable, and a
                                             * TLS read with %fs unset faults at
                                             * address 0 */
    run_init_array();                       /* constructors BEFORE main ... */
    run_ctors();                            /* ... in either scheme */
    exit(main(argc, argv, environ));
}
