#ifndef EMBLINK_SESSION_H
#define EMBLINK_SESSION_H
#include <stdint.h>
#include "../../user/lib/embk_session.h"
struct process;
int session_control(struct process *, unsigned, int);
#if defined(__x86_64__)
int session_ui_allowed(int);
uint32_t session_input_owner(void);
#else
/* Kernel screen ownership is currently x86-only; SYS_session_control reports
 * ENOTSUP on ARM. Preserve its existing compositor until that controller ports. */
static inline int session_ui_allowed(int pid) {(void)pid;return 1;}
static inline uint32_t session_input_owner(void) {return 0;}
#endif
uint32_t session_active_epoch(void);
int session_managed(void);
void session_request_lock(void);
void session_refresh_screen(void);
#endif
