/* EmbLinkOS network stack core, M1: Ethernet demux + ARP + IPv4 + ICMP echo.
 *
 * Deliberately small and synchronous. ARP/ping wait by driving virtio_net_poll()
 * themselves (in addition to the background RX kthread), so a witness thread does
 * not depend on the poller being scheduled. One NIC, static config; DHCP/DNS are
 * M2, TCP is M3, the ring-3 CAP_NETWORK surface is M4. IPs are HOST order here
 * and converted (htonl/ntohl) only at the header boundary. */

#include "net/net.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "process/process.h"   /* process_create_kthread, schedule */

struct netif g_netif;

/* ---- internet checksum (RFC 1071 / BSD in_cksum): result stored directly --- */
uint16_t net_checksum(const void *data, uint32_t len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static const uint8_t BCAST[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void ip_to_bytes(uint32_t ip, uint8_t b[4]) {
    b[0] = ip >> 24; b[1] = ip >> 16; b[2] = ip >> 8; b[3] = ip;
}
static uint32_t bytes_to_ip(const uint8_t b[4]) {
    return IPV4(b[0], b[1], b[2], b[3]);
}

/* ---- Ethernet TX -------------------------------------------------------- */
int net_tx_eth(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype,
               const void *payload, uint32_t len) {
    if (len > ETH_FRAME_MAX - ETH_HLEN) return -1;
    uint8_t frame[ETH_FRAME_MAX];
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, dst_mac, ETH_ALEN);
    memcpy(e->src, g_netif.mac, ETH_ALEN);
    e->ethertype = htons(ethertype);
    memcpy(frame + ETH_HLEN, payload, len);
    return virtio_net_tx(frame, ETH_HLEN + len);
}

/* ---- ARP ---------------------------------------------------------------- */
#define ARP_CACHE_N 16
static struct { uint32_t ip; uint8_t mac[ETH_ALEN]; bool valid; } arp_cache[ARP_CACHE_N];

static void arp_cache_put(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    int slot = -1;
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { slot = i; break; }
        if (slot < 0 && !arp_cache[i].valid) slot = i;
    }
    if (slot < 0) slot = ip % ARP_CACHE_N;      /* evict on full */
    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, ETH_ALEN);
    arp_cache[slot].valid = true;
}
static bool arp_cache_get(uint32_t ip, uint8_t mac[ETH_ALEN]) {
    for (int i = 0; i < ARP_CACHE_N; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, ETH_ALEN); return true;
        }
    return false;
}

static void arp_send(uint16_t op, uint32_t target_ip, const uint8_t target_mac[ETH_ALEN]) {
    struct arp_pkt a;
    a.htype = htons(ARP_HTYPE_ETH);
    a.ptype = htons(ETH_P_IP);
    a.hlen = ETH_ALEN; a.plen = 4;
    a.oper = htons(op);
    memcpy(a.sha, g_netif.mac, ETH_ALEN);
    ip_to_bytes(g_netif.ip, a.spa);
    memcpy(a.tha, target_mac, ETH_ALEN);
    ip_to_bytes(target_ip, a.tpa);
    net_tx_eth(op == ARP_OP_REQUEST ? BCAST : target_mac, ETH_P_ARP, &a, sizeof(a));
}

static void arp_input(const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *a = (const struct arp_pkt *)pkt;
    if (ntohs(a->ptype) != ETH_P_IP || a->plen != 4) return;

    uint32_t spa = bytes_to_ip(a->spa);
    uint32_t tpa = bytes_to_ip(a->tpa);
    arp_cache_put(spa, a->sha);                 /* learn the sender either way */

    if (ntohs(a->oper) == ARP_OP_REQUEST && tpa == g_netif.ip)
        arp_send(ARP_OP_REPLY, spa, a->sha);    /* answer "who has us" */
}

bool arp_resolve(uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    if (arp_cache_get(ip, mac_out)) return true;
    for (int attempt = 0; attempt < 4; attempt++) {
        arp_send(ARP_OP_REQUEST, ip, BCAST);
        for (int i = 0; i < 200000; i++) {
            virtio_net_poll();                  /* drive RX while we wait */
            if (arp_cache_get(ip, mac_out)) return true;
            schedule();
        }
    }
    return false;
}

/* ---- ICMP --------------------------------------------------------------- */
static volatile struct { bool waiting; uint16_t id, seq; volatile bool got; } g_ping;

static void icmp_input(uint32_t src_ip, const uint8_t *msg, uint32_t len) {
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

/* ---- IPv4 --------------------------------------------------------------- */
int net_send_ip(uint32_t dst_ip, uint8_t proto, const void *payload, uint32_t len) {
    uint8_t pkt[ETH_FRAME_MAX - ETH_HLEN];
    if (sizeof(struct ip_hdr) + len > sizeof(pkt)) return -1;

    struct ip_hdr *ip = (struct ip_hdr *)pkt;
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl = 0x45;
    ip->total_len = htons(sizeof(struct ip_hdr) + len);
    ip->ttl = 64;
    ip->proto = proto;
    ip->src = htonl(g_netif.ip);
    ip->dst = htonl(dst_ip);
    ip->checksum = 0;
    ip->checksum = net_checksum(ip, sizeof(*ip));
    memcpy(pkt + sizeof(*ip), payload, len);

    /* Next hop: on-subnet -> the host itself, else the default gateway. */
    uint32_t nexthop = ((dst_ip & g_netif.netmask) == (g_netif.ip & g_netif.netmask))
                       ? dst_ip : g_netif.gateway;
    uint8_t mac[ETH_ALEN];
    if (!arp_resolve(nexthop, mac)) return -1;
    return net_tx_eth(mac, ETH_P_IP, pkt, sizeof(struct ip_hdr) + len);
}

static void ip_input(const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct ip_hdr)) return;
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;
    if ((ip->ver_ihl >> 4) != 4) return;
    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < sizeof(struct ip_hdr) || ihl > len) return;
    if (ntohl(ip->dst) != g_netif.ip) return;   /* not for us (M1: no forwarding) */

    uint32_t src = ntohl(ip->src);
    const uint8_t *payload = pkt + ihl;
    uint32_t plen = len - ihl;
    if (ip->proto == IP_PROTO_ICMP) icmp_input(src, payload, plen);
    /* UDP/TCP: M2/M3 */
}

/* ---- Ethernet RX demux (driver -> here) --------------------------------- */
void net_rx(const uint8_t *frame, uint32_t len) {
    if (len < ETH_HLEN) return;
    const struct eth_hdr *e = (const struct eth_hdr *)frame;
    uint16_t et = ntohs(e->ethertype);
    const uint8_t *payload = frame + ETH_HLEN;
    uint32_t plen = len - ETH_HLEN;
    if (et == ETH_P_ARP) arp_input(payload, plen);
    else if (et == ETH_P_IP) ip_input(payload, plen);
}

/* ---- ICMP echo witness (M1) --------------------------------------------- */
bool net_ping(uint32_t dst_ip) {
    static uint16_t seq = 0;
    struct { struct icmp_hdr h; uint8_t data[32]; } echo;
    memset(&echo, 0, sizeof(echo));
    echo.h.type = ICMP_ECHO_REQUEST;
    echo.h.id = htons(0x1234);
    echo.h.seq = htons(++seq);
    for (uint32_t i = 0; i < sizeof(echo.data); i++) echo.data[i] = (uint8_t)i;

    g_ping.id = 0x1234; g_ping.seq = seq; g_ping.got = false; g_ping.waiting = true;
    echo.h.checksum = 0;
    echo.h.checksum = net_checksum(&echo, sizeof(echo));

    if (net_send_ip(dst_ip, IP_PROTO_ICMP, &echo, sizeof(echo)) < 0) {
        g_ping.waiting = false; return false;
    }
    for (int i = 0; i < 400000 && !g_ping.got; i++) {
        virtio_net_poll();
        schedule();
    }
    g_ping.waiting = false;
    return g_ping.got;
}

/* ---- bring-up ----------------------------------------------------------- */
static void net_rx_thread(void) {
    for (;;) { virtio_net_poll(); schedule(); }
}

void net_init(void) {
    memset(&g_netif, 0, sizeof(g_netif));
    if (!virtio_net_init(g_netif.mac)) {
        kprintf("net: no NIC -- networking disabled\n");
        return;
    }
    /* Static config for M1 (matches QEMU user-mode net / SLIRP). DHCP is M2. */
    g_netif.ip      = IPV4(10, 0, 2, 15);
    g_netif.netmask = IPV4(255, 255, 255, 0);
    g_netif.gateway = IPV4(10, 0, 2, 2);
    g_netif.up = true;
    kprintf("net: up  ip 10.0.2.15/24  gw 10.0.2.2\n");

    process_create_kthread(net_rx_thread, 0);
}
