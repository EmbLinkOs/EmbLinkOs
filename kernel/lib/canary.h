#ifndef _EMBK_CANARY_H_
#define _EMBK_CANARY_H_
#include <stdint.h>
#include "include/types.h"

/* The stack-protector guard word and its failure handler live in
 * lib/canary.c. The compiler references both by name; nothing else should.
 * These are the test hook: arm a recovery point, smash a stack on purpose,
 * and land back here instead of in a panic. */
#include "include/arch_thread.h"   /* struct kcontext, kernel_ctx_save */
extern volatile bool   g_canary_test_armed;
extern struct kcontext g_canary_test_ctx;

/* A MACRO, for the reason include/uaccess_guard.h spells out: kernel_ctx_save
 * is setjmp, and a longjmp into a function that has already returned lands in
 * a frame whose stack has been reused. As a macro the context is saved in the
 * TEST's frame, which is still live when __stack_chk_fail jumps back. The
 * first version of this was a function; it would have "recovered" into
 * garbage. */
#define canary_test_arm()                                                    \
    (kernel_ctx_save(&g_canary_test_ctx) == 0                                \
        ? (g_canary_test_armed = true, __atomic_signal_fence(__ATOMIC_SEQ_CST), true) \
        : (g_canary_test_armed = false, false))
void      canary_test_disarm(void);
uint64_t  canary_fires(void);
uintptr_t canary_value(void);

/* The victim: overruns a 16-byte local by `len - 16` bytes. NOINLINE; the
 * length arrives through a volatile so nothing can fold the overflow away. */
void      canary_smash_a_frame(int len);

#endif
