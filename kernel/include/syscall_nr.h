#ifndef _SYSCALL_NR_H_
#define _SYSCALL_NR_H_

/* The system call NUMBERS -- the wire format between ring 3 and the kernel.
 *
 * Split out of kernel/syscall/syscalls.c at docs/ARM64.md phase A5, when a
 * second architecture needed to name SYS_write and SYS_exit without pulling in
 * 2,000 lines of handlers that do not compile for it yet.
 *
 * THESE NUMBERS ARE AN ABI. Once a userland binary exists that used one, the
 * number cannot be reused for anything else. Append; never renumber, never
 * fill a hole.
 *
 * STILL DUPLICATED, and it is the next thing to fix here: user/lib/embk.h
 * carries its own copy of this list for the userspace stubs, and nothing
 * checks that the two agree. See docs/TODO.md. */

#define SYS_write   1
#define SYS_exit    2
#define SYS_yield   3
#define SYS_open    4
#define SYS_close   5
#define SYS_read    6
#define SYS_lseek   7
#define SYS_stat    8
#define SYS_readdir 9
#define SYS_spawn   10
#define SYS_wait    11
#define SYS_getpid  12
#define SYS_kill    13
#define SYS_thread_create 14
#define SYS_thread_join   15
#define SYS_thread_exit   16
#define SYS_sbrk    17
#define SYS_fstat   18
#define SYS_gettimeofday 19
#define SYS_surface_create  20
#define SYS_surface_map     21
#define SYS_surface_acquire 22
#define SYS_surface_commit  23
#define SYS_surface_release 24
#define SYS_surface_destroy 25
#define SYS_chan_pair   26
#define SYS_chan_send   27
#define SYS_chan_recv   28
#define SYS_chan_close  29
#define SYS_chan_listen  30
#define SYS_chan_accept  31
#define SYS_chan_connect 32
#define SYS_ui_present   33
#define SYS_ui_input     34
#define SYS_ui_present_rect 35
#define SYS_key_poll     36
#define SYS_key_grab     37
#define SYS_uptime_ms    38
#define SYS_win_create   39
#define SYS_win_present  40
#define SYS_win_move     41
#define SYS_win_destroy  42
#define SYS_win_create_desktop 43
#define SYS_win_input    44
#define SYS_screen_size  45
#define SYS_sleep_ms     46
#define SYS_proc_alive   47
#define SYS_win_resize   48
#define SYS_pipe         49
#define SYS_handle_close 50
#define SYS_fd_install_obj 51
#define SYS_fd_avail     52
#define SYS_unlink       53
#define SYS_mkdir        54
#define SYS_proc_list    55
#define SYS_proc_kill    56
#define SYS_rmdir        57
#define SYS_set_fs_base  58
#define SYS_cancel       59
#define SYS_cancelled    60
#define SYS_console_interrupt_route 61
#define SYS_rename       62
#define SYS_ftruncate    63
#define SYS_chmod        64
#define SYS_key_event_poll 65
#define SYS_key_mods       66
#define SYS_tty_mode       67
#define SYS_getcaps        68
/* The live-debugging contract (EMBDBG_Specification.md §6.4). Handlers live in
 * process/debug.c; must match include/debug_abi.h and the userspace mirror. */
#define SYS_debug_attach   69
#define SYS_debug_wait     70
#define SYS_debug_cont     71
#define SYS_debug_regs     72
#define SYS_debug_mem      73
#define SYS_debug_hwbp     74
#define SYS_debug_detach   75
/* Networking (M4): the ring-3 socket surface, CAP_NETWORK-gated. */
#define SYS_net_socket     76
#define SYS_net_connect    77
#define SYS_net_resolve    78
#define SYS_net_bind       79
#define SYS_net_listen     80
#define SYS_net_accept     81
#define SYS_net_sendto     82
#define SYS_net_recvfrom   83
#define SYS_fcntl          84
#define SYS_fd_poll        85
#define SYS_clip_set       86
#define SYS_clip_get       87
#define SYS_win_blur_rect  88
#define SYS_win_restore    89
#define SYS_win_minimize   90
#define SYS_screen_luma    91
#define SYS_win_desktop_front 92
/* Sound. cap_id 4 has existed in capabilities.h since the model was written
 * with nothing behind it; these are the first syscalls to gate on it. */
#define SYS_audio_open     93
#define SYS_audio_write    94
#define SYS_audio_close    95

/* Memory mappings. The kernel had only sbrk() -- one heap per process that
 * grows and never shrinks -- so before these there was no way to hand memory
 * back, and no way to ask for it with chosen permissions. See mm/vma.h. */
#define SYS_mmap           96
#define SYS_munmap         97
/* mprotect is what makes mmap's W^X refusal workable rather than merely
 * restrictive: map writable, write, then flip to executable. See mm/vma.h. */
#define SYS_mprotect       98

/* dup(oldfd, newfd, min_fd) -- one entry point for dup, dup2 and
 * fcntl(F_DUPFD). newfd < 0 means "lowest free at or above min_fd". Both
 * descriptors share ONE open file description: one cursor, one vnode
 * reference. See fs/fd.h. */
#define SYS_dup            99

/* fsync(fd) -- a REAL device flush now that write() lands in the page cache
 * instead of on the disk. It was vacuously true before and is load-bearing
 * now; see mm/vm_object.h. */
#define SYS_fsync         100

/* intr(cmd, arg) -- the COUNTED, CLEARABLE interrupt channel, as distinct from
 * cancellation. cmd 0 = take-and-clear this process's pending count; cmd 1 =
 * opt in/out of catching interrupts (arg != 0 to catch). One numbered syscall
 * with a cmd, the same shape sys_fcntl uses, because these are two halves of
 * one contract. See the intr_catch comment in process/process.h. */
#define SYS_intr          101

/* Symbolic links. A link holds TEXT, not a reference: it may name something
 * that does not exist, and is re-resolved on every walk. `readlink` reports
 * the FULL length even when the buffer is short, so a caller can tell a
 * truncated answer from a complete one. `lstat` is stat that does not follow
 * a link in the final position -- the only way to see that one IS a link. */
#define SYS_symlink       102   /* (target, linkpath)       -> 0 | -err     */
#define SYS_readlink      103   /* (path, buf, cap)         -> len | -err   */
#define SYS_lstat         104   /* (path, struct embk_stat*)-> 0 | -err     */

#endif /* _SYSCALL_NR_H_ */
