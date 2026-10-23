/* The last few pages of kernel output, kept in a ring so that a crash can
 * carry them somewhere that survives the power going off. No lock: written
 * from kprintf (which holds its own) and read from the panic path, where
 * waiting on a lock is how a crash report becomes a hang. See the .c. */
#ifndef _EMBK_KLOG_H_
#define _EMBK_KLOG_H_
#include <stdint.h>
void     klog_putc(char c);
uint32_t klog_tail(char *out, uint32_t cap);
uint64_t klog_bytes(void);
#endif
