/* Ethernet framing: build one frame around a payload and hand it to the driver.
 * The RX demux (net_rx) lives in net.c, which routes ARP/IP up by ethertype. */

#include "net/net.h"
#include "include/kstring.h"

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
