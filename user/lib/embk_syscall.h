/* user/embk_syscall.h — the raw EmbLink OS system-call ABI, shared by every
 * userland translation unit (the newlib retargeting layer user/syscalls.c,
 * and any freestanding program that wants to bypass libc). This is the ONE
 * place the syscall numbers and the int-0x80 register convention live on the
 * user side.
 *
 * THESE NUMBERS MUST MATCH kernel/cpu/syscall.c's SYS_* table exactly. There
 * is no shared kernel/user header for them yet (the kernel keeps its own
 * copy), so this is hand-synchronized -- adding or renumbering a syscall
 * means editing both. A mismatch fails silently (wrong handler runs), so
 * treat this list as load-bearing. */

#ifndef __EMBK_SYSCALL_H__
#define __EMBK_SYSCALL_H__

#include <stdint.h>
#include <stddef.h>

/* --- syscall numbers (rax) -- keep in lockstep with kernel/cpu/syscall.c --- */
#define EMBK_SYS_write          1
#define EMBK_SYS_exit           2
#define EMBK_SYS_yield          3
#define EMBK_SYS_open           4
#define EMBK_SYS_close          5
#define EMBK_SYS_read           6
#define EMBK_SYS_lseek          7
#define EMBK_SYS_stat           8
#define EMBK_SYS_readdir        9
#define EMBK_SYS_spawn          10
#define EMBK_SYS_wait           11
#define EMBK_SYS_getpid         12
#define EMBK_SYS_kill           13
#define EMBK_SYS_thread_create  14
#define EMBK_SYS_thread_join    15
#define EMBK_SYS_thread_exit    16
#define EMBK_SYS_sbrk           17
#define EMBK_SYS_fstat          18
#define EMBK_SYS_gettimeofday   19
#define EMBK_SYS_surface_create   20
#define EMBK_SYS_surface_map      21
#define EMBK_SYS_surface_acquire  22
#define EMBK_SYS_surface_commit   23
#define EMBK_SYS_surface_release  24
#define EMBK_SYS_surface_destroy  25
#define EMBK_SYS_chan_pair    26
#define EMBK_SYS_chan_send    27
#define EMBK_SYS_chan_recv    28
#define EMBK_SYS_chan_close   29
#define EMBK_SYS_chan_listen  30
#define EMBK_SYS_chan_accept  31
#define EMBK_SYS_chan_connect 32
#define EMBK_SYS_ui_present   33
#define EMBK_SYS_ui_input     34
#define EMBK_SYS_ui_present_rect 35
#define EMBK_SYS_key_poll     36
#define EMBK_SYS_key_grab     37
#define EMBK_SYS_uptime_ms    38
#define EMBK_SYS_win_create   39
#define EMBK_SYS_win_present  40
#define EMBK_SYS_win_move     41
#define EMBK_SYS_win_destroy  42
#define EMBK_SYS_win_create_desktop 43
#define EMBK_SYS_win_input    44
#define EMBK_SYS_screen_size  45
#define EMBK_SYS_sleep_ms     46
#define EMBK_SYS_proc_alive   47
#define EMBK_SYS_win_resize   48
#define EMBK_SYS_pipe         49
#define EMBK_SYS_handle_close 50
#define EMBK_SYS_fd_install_obj 51
#define EMBK_SYS_fd_avail     52
#define EMBK_SYS_unlink       53
#define EMBK_SYS_mkdir        54
#define EMBK_SYS_proc_list    55
#define EMBK_SYS_proc_kill    56
#define EMBK_SYS_rmdir        57
/* set_fs_base(addr) -- install this thread's TLS thread pointer (%fs base).
 * crt0 calls it once at startup, only when the program has a PT_TLS segment. */
#define EMBK_SYS_set_fs_base  58

/* cancel(handle) -- ask a child you hold a HANDLE for to stop; its blocking
 * syscalls then fail -EMBK_ECANCELED. cancelled() -- has THIS process been
 * cancelled? See docs/INTERRUPTION.md. */
#define EMBK_SYS_cancel       59
#define EMBK_SYS_cancelled    60

/* console_interrupt_route(handle) -- route ^C to a child you hold a handle for;
 * handle < 0 reclaims it. A delegation, not a "foreground process". */
#define EMBK_SYS_console_interrupt_route 61

/* rename(old,new) -- STRICT: fails -EEXIST if dest exists (POSIX replace is the
 * libc veneer's job). ftruncate(fd,size) -- real, over the fs truncate op. */
#define EMBK_SYS_rename       62
#define EMBK_SYS_ftruncate    63
#define EMBK_SYS_chmod        64
#define EMBK_SYS_key_event_poll 65
#define EMBK_SYS_key_mods       66

/* tty_mode(mode) -- set TTY mode; returns old mode. */
#define EMBK_SYS_tty_mode       67
#define EMBK_SYS_getcaps        68
/* (69-75 are the live-debugging contract; see kernel debug_abi.h.) */
/* Networking (M4): the ring-3 socket surface, gated on EMBK_CAP_NETWORK. */
#define EMBK_SYS_net_socket     76
#define EMBK_SYS_net_connect    77
#define EMBK_SYS_net_resolve    78
#define EMBK_SYS_net_bind       79
#define EMBK_SYS_net_listen     80
#define EMBK_SYS_net_accept     81
#define EMBK_SYS_net_sendto     82
#define EMBK_SYS_net_recvfrom   83
#define EMBK_SYS_fcntl          84   /* cmd 1 = get O_NONBLOCK, 2 = set it */
#define EMBK_SYS_fd_poll        85   /* (fd, events) -> ready POLL* bits */
#define EMBK_SYS_clip_set       86   /* (buf, len) -> 0 | -err : fill the system clipboard */
#define EMBK_SYS_clip_get       87   /* (buf, cap) -> held-len | -err : read it back */
#define EMBK_SYS_win_blur_rect  88   /* (win,x,y,w,h) -> frost the backdrop behind a sub-rect */
#define EMBK_SYS_win_restore    89   /* (spawn handle) -> un-minimize + raise that app's windows */
#define EMBK_SYS_win_minimize   90   /* (win) -> park my own window (dock click brings it back) */
#define EMBK_SYS_screen_luma    91   /* (x,y,w,h) -> mean luminance 0-255 of what is composed there */
#define EMBK_SYS_win_desktop_front 92 /* (on) -> lift the DESKTOP layer above
                                       * the app windows while a full-screen
                                       * shell surface (the launcher) is up,
                                       * and drop it back when it closes */
#define EMBK_SYS_audio_open     93
#define EMBK_SYS_audio_write    94
#define EMBK_SYS_audio_close    95
/* Memory mappings. mmap takes THREE arguments, not six: there is no `addr`
 * (MAP_FIXED is refused) and no `fd` (file-backed mappings need a page cache
 * that does not exist). A parameter that is accepted and ignored is worse than
 * one that is absent -- see kernel/mm/vma.h. */
#define EMBK_SYS_mmap           96   /* (len, prot, flags) -> address | -err */
#define EMBK_SYS_munmap         97   /* (addr, len)        -> 0       | -err */
#define EMBK_SYS_mprotect       98   /* (addr, len, prot)  -> 0       | -err */
/* dup, dup2 and fcntl(F_DUPFD) are three spellings of one operation, so they
 * are one syscall: (oldfd, newfd, min_fd), newfd < 0 meaning "lowest free at
 * or above min_fd". */
#define EMBK_SYS_dup            99   /* (oldfd, newfd, min) -> newfd  | -err */
/* A REAL device flush. write() now returns once the bytes are in the page
 * cache; this is how a caller says it needs them on the disk. */
#define EMBK_SYS_fsync         100   /* (fd)               -> 0       | -err */
/* The counted, clearable interrupt channel -- ^C that a process SURVIVES.
 * cmd 0 = take-and-clear the pending count; cmd 1 = opt in/out of catching. */
#define EMBK_SYS_intr          101   /* (cmd, arg)         -> count   | -err */
/* Symbolic links. A link holds TEXT, re-resolved on every walk -- so it may
 * name something that does not exist. readlink reports the FULL length even
 * when the buffer was short; lstat is stat that does not follow one. */
#define EMBK_SYS_symlink       102   /* (target, linkpath) -> 0       | -err */
#define EMBK_SYS_readlink      103   /* (path, buf, cap)   -> len     | -err */
#define EMBK_SYS_lstat         104   /* (path, stat*)      -> 0       | -err */
/* The SLOW path of a userland lock. An uncontended lock never gets here.
 * op 0 = WAIT (sleep if *addr == val), op 1 = WAKE (wake up to val). */
#define EMBK_SYS_futex         105   /* (addr, op, val)    -> n       | -err */

/* --- the raw trap, once per architecture ---------------------------------
 *
 * Everything ABOVE this line is machine-independent: the syscall NUMBERS are
 * the contract, and they are the same on both architectures. Only the
 * instruction that carries them into the kernel differs, so only these seven
 * wrappers are written twice. Every one returns int64_t so the kernel's
 * negative -EMBK_* error codes survive sign-extension intact.
 *
 *              | x86_64                      | aarch64
 *   trap       | int $0x80                   | svc #0
 *   number     | rax                         | x8
 *   args 1..6  | rdi rsi rdx r10 r8 r9       | x0 x1 x2 x3 x4 x5
 *   result     | rax                         | x0
 *
 * The kernel halves these mirror are kernel/arch/x86_64/syscall/syscall.c and
 * kernel/arch/aarch64/syscall/syscall.c, which both fill the same struct
 * sysargs -- so the 95 handlers never learn which of these ran. */

#if defined(__x86_64__)

/* r10/r8 (not rcx/r9) for args 4/5 -- rcx and r11 are clobbered by the
 * syscall-entry path, exactly like the Linux x86-64 int-0x80/syscall
 * convention this deliberately echoes. */

static inline int64_t embk_syscall0(int64_t n) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall1(int64_t n, int64_t a1) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall2(int64_t n, int64_t a1, int64_t a2) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall3(int64_t n, int64_t a1, int64_t a2, int64_t a3) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

#ifndef __TINYC__

static inline int64_t embk_syscall4(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4) {
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall5(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4, int64_t a5) {
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    register int64_t r8  __asm__("r8")  = a5;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall6(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4, int64_t a5, int64_t a6) {
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    register int64_t r8  __asm__("r8")  = a5;
    register int64_t r9  __asm__("r9")  = a6;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}

#else /* __TINYC__ ------------------------------------------------------------
 * TCC is the ON-OS compiler, so this header being gcc-only was an ABI bug:
 * tcc 0.9.27 does not honor `register ... asm("r10")` bindings -- it sees the
 * plain "r" constraints and its allocator runs out at the sixth operand
 * ("asm constraint 6 ('r') could not be satisfied", found by target #3,
 * EmbBuild rebuilding EmbBuild). This branch uses ONLY letter-constrained
 * operands (a/D/S/d/c -- all tcc supports) and passes the high arguments
 * through MEMORY: rcx points at a small array and the template loads
 * r10/r8/r9 itself before the int. rcx is trashed by the kernel path after
 * use, which gcc's constraint rules would object to -- gcc never sees this
 * branch. Loading zeros for unused high args is harmless: the kernel reads
 * only the registers the syscall's arity names. */

static inline int64_t embk_syscall6(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4, int64_t a5, int64_t a6) {
    int64_t ret;
    int64_t ex[3];
    ex[0] = a4; ex[1] = a5; ex[2] = a6;
    __asm__ volatile (
        "mov 0(%5), %%r10\n\t"
        "mov 8(%5), %%r8\n\t"
        "mov 16(%5), %%r9\n\t"
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "c"(ex)
        : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static inline int64_t embk_syscall4(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4) {
    return embk_syscall6(n, a1, a2, a3, a4, 0, 0);
}

static inline int64_t embk_syscall5(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4, int64_t a5) {
    return embk_syscall6(n, a1, a2, a3, a4, a5, 0);
}

#endif /* __TINYC__ */

#elif defined(__aarch64__)

/* aarch64 -- ARM64.md phase A6.
 *
 * x8 carries the number so all six argument registers stay free, which is the
 * choice Linux/aarch64 makes and a small improvement on x86, where rax is both
 * the number IN and the result OUT.
 *
 * x0 is an in-out operand ("+r") for every arity above zero: it is argument 1
 * on the way in and the result on the way out, and tying them is what stops
 * the compiler from putting the argument somewhere it would be overwritten.
 *
 * "memory" and nothing else. The kernel's vector epilogue restores the ENTIRE
 * saved frame (x0..x30) and PSTATE from SPSR_EL1, so no register and no
 * condition flag is clobbered across the trap -- x0 is written back
 * deliberately, and it is already named as an output. There is no __TINYC__
 * branch here because tcc is the ON-OS compiler and the on-OS compiler is
 * x86-only; when an aarch64 tcc exists, this is where its branch goes. */

static inline int64_t embk_syscall0(int64_t n) {
    register int64_t x8 __asm__("x8") = n;
    register int64_t x0 __asm__("x0");
    __asm__ volatile ("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline int64_t embk_syscall1(int64_t n, int64_t a1) {
    register int64_t x8 __asm__("x8") = n;
    register int64_t x0 __asm__("x0") = a1;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline int64_t embk_syscall2(int64_t n, int64_t a1, int64_t a2) {
    register int64_t x8 __asm__("x8") = n;
    register int64_t x0 __asm__("x0") = a1;
    register int64_t x1 __asm__("x1") = a2;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

static inline int64_t embk_syscall3(int64_t n, int64_t a1, int64_t a2, int64_t a3) {
    register int64_t x8 __asm__("x8") = n;
    register int64_t x0 __asm__("x0") = a1;
    register int64_t x1 __asm__("x1") = a2;
    register int64_t x2 __asm__("x2") = a3;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static inline int64_t embk_syscall4(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4) {
    register int64_t x8 __asm__("x8") = n;
    register int64_t x0 __asm__("x0") = a1;
    register int64_t x1 __asm__("x1") = a2;
    register int64_t x2 __asm__("x2") = a3;
    register int64_t x3 __asm__("x3") = a4;
    __asm__ volatile ("svc #0" : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

static inline int64_t embk_syscall5(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4, int64_t a5) {
    register int64_t x8 __asm__("x8") = n;
    register int64_t x0 __asm__("x0") = a1;
    register int64_t x1 __asm__("x1") = a2;
    register int64_t x2 __asm__("x2") = a3;
    register int64_t x3 __asm__("x3") = a4;
    register int64_t x4 __asm__("x4") = a5;
    __asm__ volatile ("svc #0" : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}

static inline int64_t embk_syscall6(int64_t n, int64_t a1, int64_t a2, int64_t a3,
                                     int64_t a4, int64_t a5, int64_t a6) {
    register int64_t x8 __asm__("x8") = n;
    register int64_t x0 __asm__("x0") = a1;
    register int64_t x1 __asm__("x1") = a2;
    register int64_t x2 __asm__("x2") = a3;
    register int64_t x3 __asm__("x3") = a4;
    register int64_t x4 __asm__("x4") = a5;
    register int64_t x5 __asm__("x5") = a6;
    __asm__ volatile ("svc #0" : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

#else
#error "embk_syscall.h: no system-call convention known for this architecture"
#endif

/* The kernel returns a small negative value (-EMBK_*, all < 4096 in
 * magnitude) on failure and a normal value (fd, byte count, address, pid,
 * 0) otherwise. This is exactly the Linux-style "errno range" convention:
 * a return in [-4095, -1] is an error code, everything else is a real
 * result -- which works uniformly for fds (small +), sizes (+), sbrk
 * addresses (large +, never in the error window), and pids. The newlib
 * stubs (user/syscalls.c) use this to decide when to set errno + return -1. */
static inline int embk_is_err(int64_t ret) {
    return ret < 0 && ret >= -4095;
}

#endif /* __EMBK_SYSCALL_H__ */
