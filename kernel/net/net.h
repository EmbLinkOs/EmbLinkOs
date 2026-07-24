#ifndef __NET_H__
#define __NET_H__

/* EmbLinkOS networking, M1: virtio-net + Ethernet/ARP/IPv4/ICMP, enough for the
 * OS to ARP-resolve a host on its segment and complete an ICMP echo round trip.
 * The userspace surface (native CAP_NETWORK endpoint handles + a sockets shim
 * for ports) is M4 -- none of this file is exposed to ring 3 yet. IPs are held
 * in HOST byte order and converted at the wire boundary (htonl/ntohl). */

#include <stdint.h>
#include "include/types.h"   /* kernel bool */

/* ---- byte order (x86 is little-endian; the wire is big-endian) ---------- */
static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

/* An IPv4 address in HOST order, e.g. 10.0.2.15 -> 0x0A00020F. */
#define IPV4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_FRAME_MAX 1514            /* 14 header + 1500 MTU */

#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;               /* network order */
} __attribute__((packed));

/* ---- ARP (IPv4 over Ethernet) ------------------------------------------- */
#define ARP_HTYPE_ETH 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct arp_pkt {
    uint16_t htype, ptype;            /* network order */
    uint8_t  hlen, plen;
    uint16_t oper;                    /* network order */
    uint8_t  sha[ETH_ALEN];
    uint8_t  spa[4];                  /* wire order (4 bytes) */
    uint8_t  tha[ETH_ALEN];
    uint8_t  tpa[4];
} __attribute__((packed));

/* ---- IPv4 --------------------------------------------------------------- */
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

struct ip_hdr {
    uint8_t  ver_ihl;                 /* 0x45 = v4, 5 words */
    uint8_t  dscp;
    uint16_t total_len;               /* network order */
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;                     /* network order */
    uint32_t dst;                     /* network order */
} __attribute__((packed));

/* ---- ICMP --------------------------------------------------------------- */
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

struct icmp_hdr {
    uint8_t  type, code;
    uint16_t checksum;
    uint16_t id, seq;                 /* network order */
} __attribute__((packed));

/* ---- UDP ---------------------------------------------------------------- */
struct udp_hdr {
    uint16_t src_port, dst_port;      /* network order */
    uint16_t len;                     /* network order: header + data */
    uint16_t checksum;
} __attribute__((packed));

/* ---- the single network interface (M1: one NIC, static config) ---------- */
struct netif {
    bool     up;
    bool     dhcp;                    /* true once a DHCP lease configured us */
    uint8_t  mac[ETH_ALEN];
    uint32_t ip;                      /* HOST order */
    uint32_t netmask;                 /* HOST order */
    uint32_t gateway;                 /* HOST order */
    uint32_t dns;                     /* HOST order (from DHCP option 6) */
};

extern struct netif g_netif;

/* Internet checksum (RFC 1071): ones-complement sum over `len` bytes. */
uint16_t net_checksum(const void *data, uint32_t len);

/* ---- stack entry points ------------------------------------------------- */
void net_init(void);                          /* bring up the NIC + static config */
void net_rx(const uint8_t *frame, uint32_t len);   /* driver -> stack (one Ethernet frame) */

/* eth: build+send a frame carrying `payload` to `dst_mac` with `ethertype`. */
int  net_tx_eth(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype,
                const void *payload, uint32_t len);

/* ARP: resolve `ip` (HOST order) to a MAC, sending a request and waiting up to
 * a bounded number of poll cycles. Returns true and fills mac on success. */
bool arp_resolve(uint32_t ip, uint8_t mac_out[ETH_ALEN]);

/* IPv4: send `payload` (an ICMP/UDP/... message) to `dst_ip` (HOST order). */
int  net_send_ip(uint32_t dst_ip, uint8_t proto, const void *payload, uint32_t len);

/* UDP: send a datagram. src_ip may be 0 (unconfigured, e.g. DHCP DISCOVER);
 * dst_ip 255.255.255.255 goes out as an Ethernet broadcast (no ARP). */
int  net_send_udp(uint32_t src_ip, uint16_t src_port,
                  uint32_t dst_ip, uint16_t dst_port,
                  const void *payload, uint32_t len);

/* DHCP: run DISCOVER/OFFER/REQUEST/ACK and, on success, configure g_netif
 * (ip/netmask/gateway/dns) and set g_netif.dhcp. Returns true on a lease. */
bool net_dhcp(void);

/* ICMP: send one echo request to `dst_ip` and wait for the matching reply.
 * Returns true on a reply (the M1 witness). */
bool net_ping(uint32_t dst_ip);

/* ---- driver interface (virtio_net.c) ------------------------------------ */
bool virtio_net_init(uint8_t mac_out[ETH_ALEN]);   /* probe + bring up; fills MAC */
int  virtio_net_tx(const void *frame, uint32_t len);   /* send one Ethernet frame */
void virtio_net_poll(void);                        /* drain the RX ring -> net_rx() */

#endif /* __NET_H__ */
