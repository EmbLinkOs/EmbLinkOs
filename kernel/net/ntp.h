/* SNTP: ask a server what time it is, because the CMOS clock is wrong and has
 * nothing to check itself against. A machine years out cannot verify a
 * certificate -- a wrong clock looks exactly like an attack. See the .c. */
#ifndef _EMBK_NTP_H_
#define _EMBK_NTP_H_
#include <stdint.h>
#include "include/types.h"

/* Ask this server (HOST order) and step the clock to its answer. */
bool net_ntp_sync(uint32_t server_ip);

/* The gateway first -- every emulator and most routers answer there, and it
 * needs nothing but a lease. pool.ntp.org by name is the fallback. */
bool net_ntp_sync_default(void);

bool     net_ntp_synced(void);
uint64_t net_ntp_last_unix(void);
uint32_t net_ntp_server(void);
int64_t  net_ntp_last_step(void);   /* how far the clock moved, in seconds */
#endif
