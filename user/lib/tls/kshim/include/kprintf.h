#ifndef EMBK_TLS_KSHIM_KPRINTF_H
#define EMBK_TLS_KSHIM_KPRINTF_H
#include <stdio.h>
/* The crypto .c reference kprintf only inside their *_run_selftests(); silence it
 * for userspace/host -- our own RFC test vectors are the validation here. */
#define kprintf(...) ((void)0)
#endif
