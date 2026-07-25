/* UDP: datagram output (with the RFC 768 pseudo-header checksum) and a single-
 * slot receive capture (udp_arm/udp_collect) that the sequential request/reply
 * clients -- DHCP, DNS -- wait on. A real per-port demux arrives with the M4
 * ring-3 endpoint API; this is deliberately just enough for those two. */

#include "net/net.h"
#include "include/kstring.h"
#include "process/process.h"   /* schedule */

int net_send_udp(uint32_t src_ip, uint16_t src_port,
                 uint32_t dst_ip, uint16_t dst_port,
                 const void *payload, uint32_t len) {
    uint8_t seg[ETH_FRAME_MAX - ETH_HLEN - sizeof(struct ip_hdr)];
    uint32_t seg_len = sizeof(struct udp_hdr) + len;
    if (seg_len > sizeof(seg)) return -1;
    struct udp_hdr *u = (struct udp_hdr *)seg;
    u->src_port = htons(src_port);
    u->dst_port = htons(dst_port);
    u->len      = htons((uint16_t)seg_len);
    u->checksum = 0;
    memcpy(seg + sizeof(*u), payload, len);
    uint16_t c = net_l4_checksum(src_ip, dst_ip, IP_PROTO_UDP, seg, seg_len);
    u->checksum = c ? c : 0xFFFF;               /* 0 means "none"; send 0xFFFF */
    return ip_output(src_ip, dst_ip, IP_PROTO_UDP, seg, seg_len);
}

#define UDP_CAP_MAX 1024
static volatile struct {
    bool     armed;
    uint16_t port;
    uint32_t from_ip;
    uint8_t  buf[UDP_CAP_MAX];
    uint32_t len;
    volatile bool got;
} g_udp;

/* ---- ring-3 UDP sockets -------------------------------------------------
 * Per-socket bound port + a small receive queue. The single-slot capture above
 * stays for the kernel's own DHCP/DNS; a bound ring-3 socket takes precedence in
 * the demux. Every public entry takes the big net lock (owner-recursive), so the
 * table is SMP-safe like the rest of the stack. */
#define UDP_SOCKS     8
#define UDP_QUEUE     4
#define UDP_DGRAM_MAX 1472       /* 1500 MTU - 20 IP - 8 UDP */

struct udp_dgram { uint32_t src_ip; uint16_t src_port; uint16_t len; uint8_t data[UDP_DGRAM_MAX]; };
struct udp_sock {
    bool in_use;
    uint16_t port;               /* bound local port (0 = unbound) */
    struct udp_dgram q[UDP_QUEUE];
    int q_head, q_n;
};
static struct udp_sock udp_socks[UDP_SOCKS];

/* Returns true if the datagram had a consumer (a bound socket or the kernel's
 * armed DHCP/DNS capture); false means the port is closed, and ip_input will
 * answer with an ICMP port-unreachable. */
bool udp_input(uint32_t src_ip, const uint8_t *seg, uint32_t seg_len) {
    if (seg_len < sizeof(struct udp_hdr)) return true;   /* malformed: swallow */
    const struct udp_hdr *u = (const struct udp_hdr *)seg;
    uint32_t dlen = ntohs(u->len);
    if (dlen < sizeof(*u) || dlen > seg_len) dlen = seg_len;
    const uint8_t *data = seg + sizeof(*u);
    uint32_t datalen = dlen - sizeof(*u);
    uint16_t dport = ntohs(u->dst_port);

    /* Ring-3 bound sockets first. (Runs under net_lock via net_rx.) */
    for (int i = 0; i < UDP_SOCKS; i++) {
        struct udp_sock *s = &udp_socks[i];
        if (s->in_use && s->port == dport) {
            if (s->q_n < UDP_QUEUE) {                /* else drop -- UDP is lossy */
                struct udp_dgram *d = &s->q[(s->q_head + s->q_n) % UDP_QUEUE];
                uint32_t k = datalen < UDP_DGRAM_MAX ? datalen : UDP_DGRAM_MAX;
                memcpy(d->data, data, k);
                d->len = (uint16_t)k; d->src_ip = src_ip; d->src_port = ntohs(u->src_port);
                s->q_n++;
            }
            return true;                             /* the port is open */
        }
    }

    /* Else the kernel single-slot capture (DHCP/DNS). */
    if (g_udp.armed && dport == g_udp.port) {
        if (!g_udp.got) {
            uint32_t k = datalen < UDP_CAP_MAX ? datalen : UDP_CAP_MAX;
            memcpy((void *)g_udp.buf, data, k);
            g_udp.len = k;
            g_udp.from_ip = src_ip;
            g_udp.got = true;
        }
        return true;
    }
    return false;                                    /* closed port -> unreachable */
}

int net_udp_open(void) {
    net_lock();
    int idx = -1;
    for (int i = 0; i < UDP_SOCKS; i++) if (!udp_socks[i].in_use) { idx = i; break; }
    if (idx >= 0) { memset(&udp_socks[idx], 0, sizeof(udp_socks[idx])); udp_socks[idx].in_use = true; }
    net_unlock();
    return idx;
}

int net_udp_bind(int us, uint16_t port) {
    if (us < 0 || us >= UDP_SOCKS) return -1;
    net_lock();
    int rc = -1;
    if (udp_socks[us].in_use) {
        bool taken = false;
        for (int i = 0; i < UDP_SOCKS; i++)
            if (i != us && udp_socks[i].in_use && port != 0 && udp_socks[i].port == port) taken = true;
        if (!taken) { udp_socks[us].port = port; rc = 0; }
    }
    net_unlock();
    return rc;
}

int net_udp_sendto(int us, uint32_t dst_ip, uint16_t dst_port, const void *data, uint32_t len) {
    if (us < 0 || us >= UDP_SOCKS) return -1;
    net_lock();
    int rc = -1;
    if (udp_socks[us].in_use) {
        if (udp_socks[us].port == 0) {               /* auto-assign an ephemeral source port */
            static uint16_t eph = 49152;
            udp_socks[us].port = eph++; if (eph == 0) eph = 49152;
        }
        rc = net_send_udp(g_netif.ip, udp_socks[us].port, dst_ip, dst_port, data, len);
    }
    net_unlock();
    return rc;
}

int net_udp_recvfrom(int us, void *buf, uint32_t cap, uint32_t *src_ip, uint16_t *src_port) {
    if (us < 0 || us >= UDP_SOCKS) return -1;
    net_lock();
    if (!udp_socks[us].in_use) { net_unlock(); return -1; }
    struct udp_sock *s = &udp_socks[us];
    int got = -1;
    for (int i = 0; i < 400000; i++) {
        if (s->q_n > 0) {
            struct udp_dgram *d = &s->q[s->q_head];
            uint32_t k = d->len < cap ? d->len : cap;
            memcpy(buf, d->data, k);
            if (src_ip)   *src_ip = d->src_ip;
            if (src_port) *src_port = d->src_port;
            s->q_head = (s->q_head + 1) % UDP_QUEUE;
            s->q_n--;
            got = (int)k;
            break;
        }
        net_yield();
    }
    net_unlock();
    return got;
}

void net_udp_close(int us) {
    if (us < 0 || us >= UDP_SOCKS) return;
    net_lock();
    udp_socks[us].in_use = false;
    net_unlock();
}

/* Lock-free free for the fd reap path (under g_sched_lock; a single bool write). */
void net_udp_abort(int us) {
    if (us < 0 || us >= UDP_SOCKS) return;
    udp_socks[us].in_use = false;
}

void udp_arm(uint16_t port) {
    g_udp.port = port; g_udp.len = 0; g_udp.from_ip = 0; g_udp.got = false; g_udp.armed = true;
}

int udp_collect(uint8_t *out, uint32_t cap, uint32_t *from_ip) {
    for (int i = 0; i < 400000 && !g_udp.got; i++) { net_yield(); }
    int rc = -1;
    if (g_udp.got) {
        uint32_t k = g_udp.len < cap ? g_udp.len : cap;
        memcpy(out, (void *)g_udp.buf, k);
        if (from_ip) *from_ip = g_udp.from_ip;
        rc = (int)k;
    }
    g_udp.armed = false;
    return rc;
}
