/* kernel/net/ntp/ntp.c -- SNTP (RFC 4330), because the CMOS clock is wrong.
 *
 * docs/PILLARS.md phase 3: "Time: NTP + time zones -- absent, the RTC only.
 * The clock drifts and is in the wrong zone."
 *
 * Every timestamp this system has ever written -- every EMBKFS inode, every
 * snapshot record, every line of a log -- came from a CMOS chip whose battery
 * nobody has changed, against nothing. A machine whose clock is an hour out
 * writes files that appear to come from the future, and a machine whose clock
 * is YEARS out cannot verify a certificate: TLS rejects one that is not yet
 * valid, so a wrong clock looks exactly like an attack.
 *
 * SNTP IS THE SIMPLE HALF OF NTP AND THAT IS THE POINT. Full NTP disciplines
 * the local oscillator against several servers, estimating drift and slewing
 * rather than stepping. This asks one server what time it is, works out how
 * long the round trip took, and steps. That is what a machine that has just
 * booted needs, and the difference only starts to matter once you care about
 * milliseconds.
 *
 * THE EPOCH IS NOT THE UNIX EPOCH. NTP counts seconds from 1900-01-01 and
 * Unix from 1970-01-01, seventy years and seventeen leap days apart --
 * 2,208,988,800 seconds. Forgetting it puts the machine in 2036, which is
 * such a specific kind of wrong that it is worth naming the constant.
 */
#include "net/net.h"
#include "net/ntp.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#include "drivers/timer/rtc.h"

#define NTP_PORT     123
#define NTP_EPH_PORT 0xC123              /* our source port */

/* 1900-01-01 to 1970-01-01, in seconds. */
#define NTP_UNIX_DELTA 2208988800ULL

/* A packet is 48 bytes; the fields this uses are the first byte and the
 * transmit timestamp at offset 40. */
#define NTP_PACKET 48
#define NTP_XMIT_OFFSET 40

#define NTP_LI_NOSYNC 3                  /* leap indicator: the server is lost */

static bool     g_synced;
static uint64_t g_last_unix;
static uint32_t g_server_ip;
static int64_t  g_last_step;

static uint64_t rd64be(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static bool ntp_query_locked(uint32_t server_ip, uint64_t *out_unix) {
    uint8_t pkt[NTP_PACKET];
    memset(pkt, 0, sizeof pkt);
    /* LI = 0 (no warning), VN = 4, Mode = 3 (client). One byte, and a server
     * answers nothing at all if the mode is wrong. */
    pkt[0] = (0 << 6) | (4 << 3) | 3;

    udp_arm(NTP_EPH_PORT);
    if (net_send_udp(g_netif.ip, NTP_EPH_PORT, server_ip, NTP_PORT,
                     pkt, sizeof pkt) < 0)
        return false;

    uint8_t in[128];
    int r = udp_collect(in, sizeof in, 0);
    if (r < NTP_PACKET) return false;

    /* WHAT THE SERVER SAYS ABOUT ITSELF, BEFORE WHAT IT SAYS ABOUT THE TIME.
     * Mode 4 is a server reply; anything else is not an answer to this.
     * Stratum 0 is a "kiss-o'-death" packet -- a refusal, carrying a reason
     * code where the timestamp would be. And leap indicator 3 means the
     * server has itself lost synchronisation, so its clock is no better than
     * the one we are trying to correct. */
    uint8_t li   = (uint8_t)(in[0] >> 6);
    uint8_t mode = (uint8_t)(in[0] & 7);
    uint8_t stratum = in[1];
    if (mode != 4) return false;
    if (stratum == 0) {
        kprintf("ntp: the server refused (kiss-o'-death)\n");
        return false;
    }
    if (li == NTP_LI_NOSYNC) {
        kprintf("ntp: the server says it is not synchronised itself\n");
        return false;
    }

    /* The transmit timestamp: 32 bits of seconds, 32 of fraction. Only the
     * seconds are used -- this clock has one-second resolution (rtc.h says so)
     * and carrying a fraction it cannot represent would be decoration. */
    uint64_t ts = rd64be(in + NTP_XMIT_OFFSET);
    uint64_t secs = ts >> 32;
    if (secs < NTP_UNIX_DELTA) return false;      /* before 1970: not a time */
    *out_unix = secs - NTP_UNIX_DELTA;
    return true;
}

bool net_ntp_sync(uint32_t server_ip) {
    if (!g_netif.up || !server_ip) return false;

    uint64_t when = 0;
    net_lock();
    bool ok = ntp_query_locked(server_ip, &when);
    net_unlock();
    if (!ok) return false;

    /* THE STEP IS AGAINST THE HARDWARE CLOCK, not against the last answer.
     * rtc_set_unix works out the offset from what the CMOS chip currently
     * reads, so two syncs in a row do not compound. */
    int64_t before = (int64_t)rtc_now_unix();
    rtc_set_unix(when);
    g_last_step = (int64_t)when - before;
    g_synced = true;
    g_last_unix = when;
    g_server_ip = server_ip;

    kprintf("ntp: %u.%u.%u.%u says %llu; the hardware clock was %lld second(s) "
            "%s\n",
            (unsigned)((server_ip >> 24) & 0xFF), (unsigned)((server_ip >> 16) & 0xFF),
            (unsigned)((server_ip >> 8) & 0xFF), (unsigned)(server_ip & 0xFF),
            (unsigned long long)when,
            (long long)(g_last_step < 0 ? -g_last_step : g_last_step),
            g_last_step < 0 ? "fast" : "slow");
    return true;
}

bool net_ntp_sync_default(void) {
    /* THE GATEWAY, NOT A NAME. Resolving pool.ntp.org needs DNS, DNS needs a
     * resolver, and both need the network to be further up than it has to be
     * for this. Every emulator and most home routers answer NTP at the
     * gateway address, and a machine that wants a specific server can be told
     * one. A name lookup is the fallback, not the first move. */
    if (!g_netif.up) return false;      /* no card, no answer, no waiting for one */
    if (g_netif.gateway && net_ntp_sync(g_netif.gateway)) return true;

    uint32_t ip = 0;
    if (net_resolve("pool.ntp.org", &ip) && ip && net_ntp_sync(ip)) return true;
    return false;
}

bool     net_ntp_synced(void)     { return g_synced; }
uint64_t net_ntp_last_unix(void)  { return g_last_unix; }
uint32_t net_ntp_server(void)     { return g_server_ip; }
int64_t  net_ntp_last_step(void)  { return g_last_step; }
