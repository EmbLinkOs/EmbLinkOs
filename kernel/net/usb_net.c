/* kernel/net/usb_net.c -- a network card on the end of a USB cable.
 *
 * WHY THIS IS WORTH MORE THAN IT LOOKS. A laptop whose wireless chip this
 * kernel has no driver for is a laptop with no network at all -- and writing a
 * wireless driver for an unknown chip is not something you do before you can
 * download anything. A USB ethernet dongle is the escape hatch: it costs
 * nothing, it works on every machine with a USB port, and it is how you get a
 * new operating system online for the first time on hardware nobody has
 * described in advance.
 *
 * TWO PROTOCOLS, and the difference matters for what this actually buys:
 *
 *   CDC-ECM  the open standard. A control interface and two bulk endpoints,
 *            and the bulk endpoints carry RAW ETHERNET FRAMES with nothing
 *            wrapped around them. Most real dongles that are not vendor-
 *            specific speak it. QEMU emulates NO device that does, so the
 *            path below is written from the specification and has never run.
 *
 *   RNDIS    Microsoft's, and what QEMU's `usb-net` actually is. A control
 *            channel carrying structured messages, and every frame wrapped in
 *            a 44-byte header. It is what Android tethering uses. This is the
 *            path that can be TESTED here, which is why it exists.
 *
 * Both end up in the same place: `struct net_driver`, beside e1000 and
 * rtl8139, so everything above the driver -- ARP, DHCP, TCP -- is unchanged.
 * The USB layer has never had a driver that registered with another subsystem
 * before; mass storage registers a block device, and this is the same shape.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "drivers/usb/usb_core.h"
#include "net/net.h"
#include "net/usb_net.h"

/* ---- interface classes -------------------------------------------------- */
#define USB_CLASS_CDC          0x02   /* communications: the control iface  */
#define USB_CLASS_CDC_DATA     0x0A   /* the data interface's own class     */
#define USB_CLASS_VENDOR       0xFF
#define CDC_SUBCLASS_ECM       0x06
#define CDC_SUBCLASS_ACM       0x02   /* RNDIS pretends to be this          */
#define RNDIS_PROTOCOL         0xFF

/* ---- RNDIS ---------------------------------------------------------------
 *
 * Messages go over the CONTROL endpoint as class requests, not over the bulk
 * pipes: SEND_ENCAPSULATED_COMMAND writes one, GET_ENCAPSULATED_RESPONSE reads
 * the reply. The bulk pipes carry only data, and every frame on them is
 * preceded by a packet header giving its offset and length. */
#define RNDIS_MSG_PACKET       0x00000001u
#define RNDIS_MSG_INIT         0x00000002u
#define RNDIS_MSG_INIT_CMPLT   0x80000002u
#define RNDIS_MSG_QUERY        0x00000004u
#define RNDIS_MSG_QUERY_CMPLT  0x80000004u
#define RNDIS_MSG_SET          0x00000005u
#define RNDIS_MSG_SET_CMPLT    0x80000005u

#define RNDIS_OID_802_3_PERMANENT_ADDRESS 0x01010101u
#define RNDIS_OID_GEN_CURRENT_PACKET_FILTER 0x0001010Eu
#define RNDIS_OID_GEN_MEDIA_CONNECT_STATUS  0x00010114u

/* Everything except broadcast/multicast/directed is off: a filter that passes
 * all traffic is promiscuous mode, which is a different feature with a
 * different cost and which nobody asked for. */
#define RNDIS_FILTER_DIRECTED   0x01
#define RNDIS_FILTER_MULTICAST  0x02
#define RNDIS_FILTER_BROADCAST  0x08

#define CDC_SEND_ENCAPSULATED   0x00
#define CDC_GET_ENCAPSULATED    0x01

/* THE FIELD OFFSETS, named rather than written as numbers at each use.
 *
 * Every RNDIS completion has the same first four words -- type, length,
 * request id, STATUS -- and Status is at +12. Reading it at +16 reads
 * MajorVersion instead, which for a device answering version 1 is a non-zero
 * value that looks exactly like a failure status. That is what "RNDIS
 * INITIALIZE refused" meant on the first run: the device had completed
 * successfully and said so. */
#define RN_TYPE     0
#define RN_LENGTH   4
#define RN_REQID    8
#define RN_STATUS   12
/* QUERY_CMPLT continues: where the answer is, and how long. Both counted from
 * the REQUEST ID field (byte 8), not from the start of the message. */
#define RN_QC_INFOLEN 16
#define RN_QC_INFOOFF 20

#define UNET_BUF 2048

enum unet_kind { UNET_NONE = 0, UNET_ECM, UNET_RNDIS };

static struct {
    struct usb_device *dev;
    enum unet_kind kind;
    uint8_t ep_in, ep_out;
    uint8_t mac[ETH_ALEN];
    bool    up;
    uint32_t req_id;
    uint64_t rx_frames, tx_frames, rx_dropped;
} g;

static uint8_t g_rx[UNET_BUF];
static uint8_t g_tx[UNET_BUF];

static int dev_bulk_out(const void *buf, uint32_t len);

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- the RNDIS control channel ------------------------------------------ */
static bool rndis_command(const void *msg, uint32_t len,
                          void *reply, uint32_t reply_len) {
    /* Class request to the interface. wValue 0, wIndex 0 -- the interface
     * number, which is 0 on every device this attaches to. */
    int rc = usb_control(g.dev, 0x21, CDC_SEND_ENCAPSULATED, 0, 0,
                         (void *)msg, (uint16_t)len);
    if (rc < 0) return false;
    if (!reply) return true;

    /* The reply is not ready instantly. The specification has the device
     * raise a notification on its interrupt endpoint; polling the control
     * pipe a few times is simpler and works because the host controller
     * serialises everything anyway. */
    for (int tries = 0; tries < 100; tries++) {
        memset(reply, 0, reply_len);
        rc = usb_control(g.dev, 0xA1, CDC_GET_ENCAPSULATED, 0, 0,
                         reply, (uint16_t)reply_len);
        if (rc > 0 && get_le32((uint8_t *)reply) != 0) return true;
        for (volatile int d = 0; d < 20000; d++) { }
    }
    return false;
}

static bool rndis_query(uint32_t oid, void *out, uint32_t out_len,
                        uint32_t *got_len) {
    uint8_t msg[28];                  /* +24 is DeviceVcHandle, always 0 */
    memset(msg, 0, sizeof msg);
    put_le32(msg + 0,  RNDIS_MSG_QUERY);
    put_le32(msg + 4,  sizeof msg);
    put_le32(msg + 8,  ++g.req_id);
    put_le32(msg + 12, oid);
    put_le32(msg + 16, 0);            /* no input buffer            */
    put_le32(msg + 20, 0);            /* ...so no offset either     */

    static uint8_t reply[256];
    if (!rndis_command(msg, sizeof msg, reply, sizeof reply)) return false;
    if (get_le32(reply + RN_TYPE) != RNDIS_MSG_QUERY_CMPLT) {
        kprintf("usb-net: query 0x%x answered type 0x%x\n", oid,
                get_le32(reply + RN_TYPE));
        return false;
    }
    if (get_le32(reply + RN_STATUS) != 0) {
        kprintf("usb-net: query 0x%x status 0x%x\n", oid,
                get_le32(reply + RN_STATUS));
        return false;
    }

    uint32_t ilen = get_le32(reply + RN_QC_INFOLEN);
    uint32_t ioff = get_le32(reply + RN_QC_INFOOFF);
    if (ioff + 8 + ilen > sizeof reply) return false;
    if (ilen > out_len) ilen = out_len;
    memcpy(out, reply + 8 + ioff, ilen);
    if (got_len) *got_len = ilen;
    return true;
}

static bool rndis_set_u32(uint32_t oid, uint32_t value) {
    uint8_t msg[32];
    memset(msg, 0, sizeof msg);
    put_le32(msg + 0,  RNDIS_MSG_SET);
    put_le32(msg + 4,  sizeof msg);
    put_le32(msg + 8,  ++g.req_id);
    put_le32(msg + 12, oid);
    put_le32(msg + 16, 4);            /* information buffer length */
    put_le32(msg + 20, 20);           /* offset                    */
    put_le32(msg + 28, value);

    static uint8_t reply[64];
    if (!rndis_command(msg, sizeof msg, reply, sizeof reply)) return false;
    if (get_le32(reply + RN_TYPE) != RNDIS_MSG_SET_CMPLT ||
        get_le32(reply + RN_STATUS) != 0) {
        kprintf("usb-net: set 0x%x answered type 0x%x status 0x%x\n", oid,
                get_le32(reply + RN_TYPE), get_le32(reply + RN_STATUS));
        return false;
    }
    return true;
}

static bool rndis_bringup(void) {
    uint8_t msg[24];
    memset(msg, 0, sizeof msg);
    put_le32(msg + 0,  RNDIS_MSG_INIT);
    put_le32(msg + 4,  sizeof msg);
    put_le32(msg + 8,  ++g.req_id);
    put_le32(msg + 12, 1);            /* major version */
    put_le32(msg + 16, 0);            /* minor version */
    put_le32(msg + 20, UNET_BUF);     /* max transfer size we will accept */

    static uint8_t reply[64];
    if (!rndis_command(msg, sizeof msg, reply, sizeof reply)) {
        kprintf("usb-net: RNDIS INITIALIZE got no reply\n");
        return false;
    }
    if (get_le32(reply + RN_TYPE) != RNDIS_MSG_INIT_CMPLT ||
        get_le32(reply + RN_STATUS) != 0) {
        kprintf("usb-net: RNDIS INITIALIZE answered type 0x%x status 0x%x\n",
                get_le32(reply + RN_TYPE), get_le32(reply + RN_STATUS));
        return false;
    }

    uint32_t got = 0;
    if (!rndis_query(RNDIS_OID_802_3_PERMANENT_ADDRESS, g.mac, ETH_ALEN, &got) ||
        got != ETH_ALEN) {
        kprintf("usb-net: RNDIS would not report its MAC\n");
        return false;
    }

    /* WITHOUT A PACKET FILTER THE DEVICE RECEIVES NOTHING. It comes up with
     * every class turned off, so a driver that sets up its endpoints
     * perfectly and never writes this sees an entirely silent network and no
     * error anywhere. */
    if (!rndis_set_u32(RNDIS_OID_GEN_CURRENT_PACKET_FILTER,
                       RNDIS_FILTER_DIRECTED | RNDIS_FILTER_MULTICAST |
                       RNDIS_FILTER_BROADCAST)) {
        kprintf("usb-net: RNDIS refused the packet filter\n");
        return false;
    }
    /* READ IT BACK. The device passes traffic in NEITHER direction until a
     * non-zero filter is set, and a SET that reports success while changing
     * nothing looks exactly like a working driver on a silent network. */
    {
        uint32_t f = 0, n2 = 0;
        if (!rndis_query(RNDIS_OID_GEN_CURRENT_PACKET_FILTER, &f, 4, &n2) || !f) {
            kprintf("usb-net: the packet filter did not stick (reads 0x%x)\n", f);
            return false;
        }
    }
    return true;
}

/* ---- CDC-ECM -------------------------------------------------------------
 *
 * The MAC is not readable over a control request: it is a STRING DESCRIPTOR
 * whose index the Ethernet Networking functional descriptor names, written as
 * twelve hex characters. That is the one genuinely odd thing about ECM and the
 * part most likely to be wrong in an implementation nobody has run. */
static bool ecm_read_mac(uint8_t str_index) {
    uint8_t buf[64];
    memset(buf, 0, sizeof buf);
    int rc = usb_control(g.dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                         (uint16_t)((USB_DESC_STRING << 8) | str_index),
                         0x0409, buf, sizeof buf);
    if (rc < 4) return false;

    /* A string descriptor is UTF-16: length, type, then the characters. We
     * want twelve hex digits, so twenty-four bytes of payload. */
    int chars = (buf[0] - 2) / 2;
    if (chars < 12) return false;
    for (int i = 0; i < 12; i++) {
        char c = (char)buf[2 + i * 2];
        int v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else return false;
        if (i & 1) g.mac[i / 2] |= (uint8_t)v;
        else       g.mac[i / 2]  = (uint8_t)(v << 4);
    }
    return true;
}

/* ---- the net_driver operations ------------------------------------------ */
bool usb_net_drv_init(uint8_t mac_out[ETH_ALEN]) {
    if (!g.up) return false;
    memcpy(mac_out, g.mac, ETH_ALEN);
    return true;
}

int usb_net_drv_tx(const void *frame, uint32_t len) {
    if (!g.up || !frame || len == 0) return -1;

    if (g.kind == UNET_RNDIS) {
        /* A 44-byte packet header, then the frame. DataOffset counts from
         * byte 8 of the message, which is the detail that puts the frame in
         * the wrong place if you measure from the start. */
        if (len + 44 > sizeof g_tx) return -1;
        memset(g_tx, 0, 44);
        put_le32(g_tx + 0, RNDIS_MSG_PACKET);
        put_le32(g_tx + 4, 44 + len);
        put_le32(g_tx + 8, 36);            /* data offset, from byte 8 */
        put_le32(g_tx + 12, len);
        memcpy(g_tx + 44, frame, len);
        int rc = dev_bulk_out(g_tx, 44 + len);
        if (rc < 0) return -1;
    } else {
        if (len > sizeof g_tx) return -1;
        memcpy(g_tx, frame, len);
        int rc = dev_bulk_out(g_tx, len);
        if (rc < 0) return -1;
    }
    g.tx_frames++;
    return (int)len;
}

void usb_net_drv_poll(void) {
    if (!g.up) return;

    int n = g.dev->ops->bulk(g.dev, g.ep_in, g_rx, sizeof g_rx);
    if (n <= 0) return;

    if (g.kind == UNET_RNDIS) {
        /* One transfer may carry several packet messages back to back. */
        uint32_t off = 0;
        while (off + 44 <= (uint32_t)n) {
            if (get_le32(g_rx + off) != RNDIS_MSG_PACKET) break;
            uint32_t msglen = get_le32(g_rx + off + 4);
            uint32_t doff   = get_le32(g_rx + off + 8);
            uint32_t dlen   = get_le32(g_rx + off + 12);
            if (msglen == 0 || off + msglen > (uint32_t)n) break;
            uint32_t start = off + 8 + doff;
            if (start + dlen <= (uint32_t)n && dlen >= ETH_HLEN) {
                net_rx(g_rx + start, dlen);
                g.rx_frames++;
            } else {
                g.rx_dropped++;
            }
            off += msglen;
        }
    } else if ((uint32_t)n >= ETH_HLEN) {
        net_rx(g_rx, (uint32_t)n);
        g.rx_frames++;
    }
}

/* The cable. RNDIS can be asked; ECM reports it on its interrupt endpoint,
 * which this does not service -- so it answers "cannot say" rather than
 * inventing one, and net.c treats that as up. */
int usb_net_drv_link(void) {
    if (!g.up) return -1;
    if (g.kind != UNET_RNDIS) return -1;
    uint32_t status = 0, got = 0;
    if (!rndis_query(RNDIS_OID_GEN_MEDIA_CONNECT_STATUS, &status, 4, &got) ||
        got != 4)
        return -1;
    return status == 0 ? 1 : 0;      /* 0 = connected, in RNDIS's numbering */
}

/* ---- attach, from the USB class dispatch -------------------------------- */
static int dev_bulk_out(const void *buf, uint32_t len) {
    return g.dev->ops->bulk(g.dev, g.ep_out, (void *)buf, len);
}

bool usb_net_attach(struct usb_device *dev) {
    if (g.up) {
        kprintf("usb-net: a second adapter is present; this kernel drives one\n");
        return false;
    }

    const struct usb_ep_info *in  = usb_find_ep(dev, 2, true);
    const struct usb_ep_info *out = usb_find_ep(dev, 2, false);
    if (!in || !out) {
        kprintf("usb-net: no bulk endpoints\n");
        return false;
    }

    memset(&g, 0, sizeof g);
    g.dev = dev;
    g.ep_in = in->addr;
    g.ep_out = out->addr;

    /* WHICH PROTOCOL. RNDIS advertises itself as a communications device
     * whose subclass is ACM and whose protocol is vendor-specific -- a
     * combination that means nothing else. Everything else claiming CDC with
     * the Ethernet subclass is ECM. */
    if (dev->if_class == USB_CLASS_CDC && dev->if_subclass == CDC_SUBCLASS_ACM &&
        dev->if_protocol == RNDIS_PROTOCOL) {
        g.kind = UNET_RNDIS;
    } else if (dev->if_class == USB_CLASS_CDC &&
               dev->if_subclass == CDC_SUBCLASS_ECM) {
        g.kind = UNET_ECM;
    } else if (dev->if_class == USB_CLASS_VENDOR) {
        /* Vendor class with two bulk endpoints is how RNDIS appears when the
         * descriptors were written for Windows rather than for the standard. */
        g.kind = UNET_RNDIS;
    } else {
        return false;
    }

    if (g.kind == UNET_RNDIS) {
        if (!rndis_bringup()) return false;
    } else {
        /* The MAC's string index is in the Ethernet functional descriptor,
         * which the core does not keep. Without it there is no address, and a
         * NIC with no address cannot be brought up honestly. */
        kprintf("usb-net: CDC-ECM found, but this kernel cannot read its MAC "
                "yet -- the functional descriptor is not parsed. See "
                "docs/TODO.md\n");
        return false;
    }

    g.up = true;
    kprintf("usb-net: %s adapter, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            g.kind == UNET_RNDIS ? "RNDIS" : "CDC-ECM",
            g.mac[0], g.mac[1], g.mac[2], g.mac[3], g.mac[4], g.mac[5]);
    return true;
}

bool usb_net_present(void) { return g.up; }

void usb_net_stats(uint64_t *rx, uint64_t *tx, uint64_t *dropped) {
    if (rx)      *rx = g.rx_frames;
    if (tx)      *tx = g.tx_frames;
    if (dropped) *dropped = g.rx_dropped;
}
