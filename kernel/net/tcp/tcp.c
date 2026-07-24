/* TCP (RFC 793), M3 -- an active-open client, enough for the OS to make a real
 * connection: 3-way handshake, in-order data in/out, and a clean FIN close.
 *
 * Scope is deliberate. It is STOP-AND-WAIT (one unacked segment at a time) with
 * simple timeout retransmit, no congestion control, no out-of-order reassembly
 * (SLIRP/loopback deliver in order; anything else is dropped and re-ACKed so the
 * peer resends). Passive open (listen/accept), windows/pipelining, and the
 * ring-3 surface are later work. Like the rest of the stack it is synchronous:
 * the blocking calls drive virtio_net_poll() while they wait.
 *
 * State lives in a small TCB table; tcp_input() runs the state machine, the
 * net_tcp_* calls are the client API. Sequence math uses serial-number
 * comparison so it is correct across the 32-bit wrap. */

#include "net/net.h"
#include "include/kstring.h"
#include "process/process.h"   /* schedule */

#define TCP_CONNS   4
#define TCP_RXBUF   16384
#define TCP_MSS     1400       /* conservative; avoids IP fragmentation */
#define SPIN_MAX    600000     /* poll iterations before a retransmit/timeout */
#define RETRIES     6

enum tcp_state {
    TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSING,
    TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_TIME_WAIT,
};

struct tcb {
    bool     in_use;
    int      state;
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    uint32_t snd_una;      /* oldest unacked seq */
    uint32_t snd_nxt;      /* next seq to send */
    uint32_t rcv_nxt;      /* next seq we expect */
    bool     peer_fin;     /* saw the peer's FIN */
    bool     reset;        /* saw RST */
    uint8_t  rxbuf[TCP_RXBUF];
    uint32_t rx_len;
};

static struct tcb tcbs[TCP_CONNS];

/* Serial-number comparison (RFC 1982): correct across the u32 wrap. */
#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) <  0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) >  0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

static struct tcb *tcb_find(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    for (int i = 0; i < TCP_CONNS; i++) {
        struct tcb *t = &tcbs[i];
        if (t->in_use && t->local_port == local_port &&
            t->remote_ip == remote_ip && t->remote_port == remote_port)
            return t;
    }
    return 0;
}

/* Send one segment carrying `flags` and `len` data bytes at sequence `seq`. A
 * pure ACK/FIN/SYN uses len 0. Window advertises our free receive room. */
static int tcp_seg(struct tcb *t, uint8_t flags, uint32_t seq,
                   const uint8_t *data, uint32_t len) {
    uint8_t seg[sizeof(struct tcp_hdr) + TCP_MSS];
    if (len > TCP_MSS) len = TCP_MSS;
    struct tcp_hdr *th = (struct tcp_hdr *)seg;
    memset(th, 0, sizeof(*th));
    th->src_port = htons(t->local_port);
    th->dst_port = htons(t->remote_port);
    th->seq      = htonl(seq);
    th->ack      = htonl(t->rcv_nxt);
    th->data_off = (sizeof(struct tcp_hdr) / 4) << 4;
    th->flags    = flags;
    uint32_t room = TCP_RXBUF - t->rx_len;
    th->window   = htons(room > 65535 ? 65535 : (uint16_t)room);
    if (len) memcpy(seg + sizeof(*th), data, len);
    th->checksum = net_l4_checksum(t->local_ip, t->remote_ip, IP_PROTO_TCP, seg, sizeof(*th) + len);
    return ip_output(t->local_ip, t->remote_ip, IP_PROTO_TCP, seg, sizeof(*th) + len);
}

void tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t seg_len) {
    if (seg_len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *th = (const struct tcp_hdr *)seg;
    uint32_t doff = (th->data_off >> 4) * 4;
    if (doff < sizeof(*th) || doff > seg_len) return;

    uint16_t sport = ntohs(th->src_port), dport = ntohs(th->dst_port);
    uint8_t  flags = th->flags;
    uint32_t seq = ntohl(th->seq), ack = ntohl(th->ack);
    const uint8_t *data = seg + doff;
    uint32_t datalen = seg_len - doff;

    struct tcb *t = tcb_find(dport, src_ip, sport);
    if (!t) return;                       /* no such connection (M3: active opens only) */

    if (flags & TCP_RST) { t->reset = true; t->state = TCP_CLOSED; return; }

    /* Advance snd_una on a valid ACK (any state past SYN). */
    if ((flags & TCP_ACK) && SEQ_GT(ack, t->snd_una) && SEQ_LEQ(ack, t->snd_nxt))
        t->snd_una = ack;

    switch (t->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == t->snd_nxt) {
            t->rcv_nxt = seq + 1;         /* SYN consumes one sequence number */
            t->snd_una = ack;
            t->state = TCP_ESTABLISHED;
            tcp_seg(t, TCP_ACK, t->snd_nxt, 0, 0);   /* complete the handshake */
        }
        return;

    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        if (datalen && seq == t->rcv_nxt) {          /* in-order data */
            uint32_t room = TCP_RXBUF - t->rx_len;
            uint32_t k = datalen < room ? datalen : room;
            memcpy(t->rxbuf + t->rx_len, data, k);
            t->rx_len   += k;
            t->rcv_nxt  += k;
        } else if (datalen && SEQ_LT(seq, t->rcv_nxt)) {
            tcp_seg(t, TCP_ACK, t->snd_nxt, 0, 0);   /* duplicate: re-ACK */
        }
        if ((flags & TCP_FIN) && seq + datalen == t->rcv_nxt) {
            t->rcv_nxt += 1;                          /* FIN consumes one seq */
            t->peer_fin = true;
            if (t->state == TCP_ESTABLISHED)      t->state = TCP_CLOSE_WAIT;
            else if (t->state == TCP_FIN_WAIT_1)  t->state = TCP_CLOSING;
            else if (t->state == TCP_FIN_WAIT_2)  t->state = TCP_TIME_WAIT;
        }
        if (datalen || (flags & TCP_FIN))
            tcp_seg(t, TCP_ACK, t->snd_nxt, 0, 0);   /* ACK what we accepted */

        /* Our own FIN getting acked. */
        if (t->state == TCP_FIN_WAIT_1 && t->snd_una == t->snd_nxt) t->state = TCP_FIN_WAIT_2;
        if (t->state == TCP_CLOSING     && t->snd_una == t->snd_nxt) t->state = TCP_TIME_WAIT;
        return;

    case TCP_LAST_ACK:
        if (t->snd_una == t->snd_nxt) t->state = TCP_CLOSED;
        return;
    default:
        return;
    }
}

/* ---- blocking client API ------------------------------------------------ */
static bool poll_until(struct tcb *t, int want_state_reached) {
    for (int i = 0; i < SPIN_MAX; i++) {
        if (t->reset) return false;
        if (want_state_reached && t->state == want_state_reached) return true;
        virtio_net_poll();
        schedule();
    }
    return false;
}

int net_tcp_connect(uint32_t dst_ip, uint16_t dst_port) {
    static uint16_t next_port = 40000;
    static uint32_t next_isn  = 0x1000;

    int idx = -1;
    for (int i = 0; i < TCP_CONNS; i++) if (!tcbs[i].in_use) { idx = i; break; }
    if (idx < 0) return -1;

    struct tcb *t = &tcbs[idx];
    memset(t, 0, sizeof(*t));
    t->in_use = true;
    t->local_ip = g_netif.ip;   t->remote_ip = dst_ip;
    t->local_port = next_port++; t->remote_port = dst_port;
    next_isn += 0x9E3F;                          /* bump the ISN each connection */
    t->snd_una = t->snd_nxt = next_isn;
    t->state = TCP_SYN_SENT;

    for (int r = 0; r < RETRIES; r++) {
        tcp_seg(t, TCP_SYN, t->snd_una, 0, 0);   /* (re)send SYN at the ISN */
        t->snd_nxt = t->snd_una + 1;             /* SYN consumes one seq */
        if (poll_until(t, TCP_ESTABLISHED)) return idx;
        if (t->reset) break;
    }
    t->in_use = false;
    return -1;
}

int net_tcp_send(int conn, const void *data, uint32_t len) {
    if (conn < 0 || conn >= TCP_CONNS || !tcbs[conn].in_use) return -1;
    struct tcb *t = &tcbs[conn];
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) return -1;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent; if (chunk > TCP_MSS) chunk = TCP_MSS;
        uint32_t seq = t->snd_nxt;
        int acked = 0;
        for (int r = 0; r < RETRIES && !acked; r++) {
            tcp_seg(t, TCP_PSH | TCP_ACK, seq, p + sent, chunk);
            if (r == 0) t->snd_nxt = seq + chunk;
            for (int i = 0; i < SPIN_MAX; i++) {
                if (t->reset) return sent ? (int)sent : -1;
                if (SEQ_GEQ(t->snd_una, seq + chunk)) { acked = 1; break; }
                virtio_net_poll(); schedule();
            }
        }
        if (!acked) return sent ? (int)sent : -1;
        sent += chunk;
    }
    return (int)sent;
}

int net_tcp_recv(int conn, void *buf, uint32_t cap) {
    if (conn < 0 || conn >= TCP_CONNS || !tcbs[conn].in_use) return -1;
    struct tcb *t = &tcbs[conn];

    for (int i = 0; i < SPIN_MAX && t->rx_len == 0; i++) {
        if (t->peer_fin || t->reset || t->state == TCP_CLOSED) break;
        virtio_net_poll(); schedule();
    }
    uint32_t k = t->rx_len < cap ? t->rx_len : cap;
    if (k) {
        memcpy(buf, t->rxbuf, k);
        if (k < t->rx_len) memmove(t->rxbuf, t->rxbuf + k, t->rx_len - k);
        t->rx_len -= k;
    }
    return (int)k;                               /* 0 = EOF (peer FIN, no data left) */
}

void net_tcp_close(int conn) {
    if (conn < 0 || conn >= TCP_CONNS || !tcbs[conn].in_use) return;
    struct tcb *t = &tcbs[conn];

    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
        int last = (t->state == TCP_CLOSE_WAIT);
        uint32_t seq = t->snd_nxt;
        t->state = last ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
        tcp_seg(t, TCP_FIN | TCP_ACK, seq, 0, 0);
        t->snd_nxt = seq + 1;                    /* FIN consumes one seq */
        /* Best-effort wait for the close to settle (not required for correctness
         * on our side; SLIRP tears down regardless). */
        for (int i = 0; i < SPIN_MAX; i++) {
            if (t->state == TCP_CLOSED || t->state == TCP_TIME_WAIT ||
                t->state == TCP_FIN_WAIT_2 || t->reset) break;
            virtio_net_poll(); schedule();
        }
    }
    t->in_use = false;
}
