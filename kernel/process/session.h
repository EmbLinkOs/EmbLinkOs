#ifndef EMBLINK_SESSION_H
#define EMBLINK_SESSION_H
#include <stdint.h>
#include "../../user/lib/embk_session.h"
struct process;
void session_bind_init(uint32_t);
int session_control(struct process *, unsigned, int);
int session_ui_allowed(int);
uint32_t session_input_owner(void);
uint32_t session_active_epoch(void);
int session_managed(void);
int session_may_spawn(const struct process *);
int session_may_signal(const struct process *,uint32_t);
void session_request_lock(void);
void session_refresh_screen(void);
#endif
