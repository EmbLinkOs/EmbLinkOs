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

void udp_input(uint32_t src_ip, const uint8_t *seg, uint32_t seg_len) {
    if (seg_len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr *u = (const struct udp_hdr *)seg;
    uint32_t dlen = ntohs(u->len);
    if (dlen < sizeof(*u) || dlen > seg_len) dlen = seg_len;
    const uint8_t *data = seg + sizeof(*u);
    uint32_t datalen = dlen - sizeof(*u);
    if (g_udp.armed && !g_udp.got && ntohs(u->dst_port) == g_udp.port) {
        uint32_t k = datalen < UDP_CAP_MAX ? datalen : UDP_CAP_MAX;
        memcpy((void *)g_udp.buf, data, k);
        g_udp.len = k;
        g_udp.from_ip = src_ip;
        g_udp.got = true;
    }
}

void udp_arm(uint16_t port) {
    g_udp.port = port; g_udp.len = 0; g_udp.from_ip = 0; g_udp.got = false; g_udp.armed = true;
}

int udp_collect(uint8_t *out, uint32_t cap, uint32_t *from_ip) {
    for (int i = 0; i < 400000 && !g_udp.got; i++) { virtio_net_poll(); schedule(); }
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
