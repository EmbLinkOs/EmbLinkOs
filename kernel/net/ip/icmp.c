/* ICMP over IPv4: reflect inbound echo requests (so the OS answers pings) and
 * match echo replies to an outbound net_ping() -- the M1 witness. */

#include "net/net.h"
#include "include/kstring.h"
#include "process/process.h"   /* schedule */

static volatile struct { bool waiting; uint16_t id, seq; volatile bool got; } g_ping;

void icmp_input(uint32_t src_ip, const uint8_t *msg, uint32_t len) {
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *ih = (const struct icmp_hdr *)msg;

    if (ih->type == ICMP_ECHO_REQUEST) {
        /* Reflect it: same id/seq/data, type -> reply, fresh checksum. */
        uint8_t buf[ETH_FRAME_MAX - ETH_HLEN - sizeof(struct ip_hdr)];
        if (len > sizeof(buf)) return;
        memcpy(buf, msg, len);
        struct icmp_hdr *r = (struct icmp_hdr *)buf;
        r->type = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = net_checksum(buf, len);
        net_send_ip(src_ip, IP_PROTO_ICMP, buf, len);
    } else if (ih->type == ICMP_ECHO_REPLY) {
        if (g_ping.waiting && ntohs(ih->id) == g_ping.id && ntohs(ih->seq) == g_ping.seq)
            g_ping.got = true;
    }
}

bool net_ping(uint32_t dst_ip) {
    static uint16_t seq = 0;
    struct { struct icmp_hdr h; uint8_t data[32]; } echo;
    memset(&echo, 0, sizeof(echo));
    echo.h.type = ICMP_ECHO_REQUEST;
    echo.h.id = htons(0x1234);
    echo.h.seq = htons(++seq);
    for (uint32_t i = 0; i < sizeof(echo.data); i++) echo.data[i] = (uint8_t)i;

    net_lock();
    g_ping.id = 0x1234; g_ping.seq = seq; g_ping.got = false; g_ping.waiting = true;
    echo.h.checksum = 0;
    echo.h.checksum = net_checksum(&echo, sizeof(echo));

    if (net_send_ip(dst_ip, IP_PROTO_ICMP, &echo, sizeof(echo)) < 0) {
        g_ping.waiting = false; net_unlock(); return false;
    }
    for (int i = 0; i < 400000 && !g_ping.got; i++) {
        net_yield();
    }
    g_ping.waiting = false;
    bool got = g_ping.got;
    net_unlock();
    return got;
}
