/* The first program EmbLinkOS ever runs at EL0 on aarch64.
 * docs/ARM64.md phase A5.
 *
 * Hand-written assembly, and deliberately so: A6 brings up the newlib
 * toolchain, and until it exists this is the only way to have user code at all.
 * It is linked at address 0 and mapped wherever the kernel chooses, so every
 * reference has to be PC-relative -- `adr`, not a literal pool holding an
 * absolute address. (The x86 side learned the same lesson: see the note in
 * usermode.c about `movabs` versus `lea msg(%rip)`.)
 *
 * The calling convention it exercises is the one kernel/arch/aarch64/syscall/
 * syscall.c implements: number in x8, arguments in x0..x5, result in x0.
 * x8 rather than x0 because all six argument registers should stay free --
 * the same reason Linux/aarch64 chose it, and unlike x86 where the number
 * shares rax with the return value.
 *
 * It tests four things, in an order where each one's failure is distinct:
 *   1. a syscall with arguments, that succeeds        (write)
 *   2. a syscall with no arguments                    (getpid)
 *   3. a syscall the kernel must REFUSE               (write from an unmapped
 *      pointer -- the privilege boundary, checked)
 *   4. a syscall that does not return                 (exit, with a value the
 *      kernel checks, so argument passing is proven end to end)
 */

    .text
    .globl _probe_start
_probe_start:

    /* --- 1. write(1, msg, len) --------------------------------------------- */
    mov     x0, #1                          /* fd = 1 (the serial console)     */
    adr     x1, probe_msg                   /* PC-relative: survives the copy  */
    mov     x2, #(probe_msg_end - probe_msg)
    mov     x8, #1                          /* SYS_write                       */
    svc     #0
    mov     x19, x0                         /* bytes written                   */

    /* --- 2. getpid(), no arguments ------------------------------------------ */
    mov     x8, #12                         /* SYS_getpid                      */
    svc     #0
    mov     x20, x0

    /* --- 3. write() from an address that is NOT mapped ---------------------
     * The kernel must return -EFAULT rather than faulting. If the boundary is
     * missing, this is where the kernel dies -- which is exactly why the test
     * comes before exit() and not after. */
    mov     x0, #1
    mov     x1, #0x1000                     /* unmapped user address           */
    mov     x2, #8
    mov     x8, #1                          /* SYS_write                       */
    svc     #0
    mov     x21, x0                         /* expected: -EMBK_EFAULT (-14)    */

    /* --- 4. exit(42) --------------------------------------------------------
     * 42 is arbitrary and that is the point: the kernel asserts it saw 42, so
     * a syscall argument has provably travelled from an EL0 register through
     * struct sysargs into a handler. */
    mov     x0, #42
    mov     x8, #2                          /* SYS_exit                        */
    svc     #0

    /* exit() does not return. If it does, stop here rather than running off
     * the end of the mapping into whatever follows. */
1:  b       1b

probe_msg:
    .ascii  "hello from EL0 -- aarch64 user mode\n"
probe_msg_end:
