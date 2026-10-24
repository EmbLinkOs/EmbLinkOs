#ifndef EMBK_PTY_H
#define EMBK_PTY_H
#include "include/types.h"
#include <stddef.h>
#include <stdint.h>
#include "../../user/lib/embk_terminal.h"

struct pty;
int pty_create(struct pty **out);
void pty_ref_locked(struct pty *p, int side);
void pty_unref_locked(struct pty *p, int side);
void pty_pin_locked(struct pty *p);
void pty_unpin_locked(struct pty *p);
int pty_read(struct pty *, int side, void *, size_t, size_t *, bool nonblock);
int pty_write(struct pty *, int side, const void *, size_t, size_t *, bool nonblock);
int64_t pty_avail(struct pty *, int side);
int pty_poll(struct pty *, int side, int events);
int pty_control(struct pty *, int side, unsigned op, void *data);
unsigned pty_live_count(void);
#endif
