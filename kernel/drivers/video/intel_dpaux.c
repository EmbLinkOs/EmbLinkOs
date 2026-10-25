/* intel_dpaux.c -- DisplayPort AUX on Intel Gen9 display (Skylake to Comet
 * Lake). Register layout and the send-control word follow Intel's Gen9
 * display programming reference and Linux's i915 (skl_get_aux_send_ctl);
 * the message format is VESA DisplayPort 1.4 section 2.7.
 *
 * Polled, never interrupt-driven: the kernel takes no display interrupts,
 * and the interrupt-enable bit is left clear so an AUX completion cannot
 * assert a line nobody services. */
#include "drivers/video/intel_dpaux.h"
#include "include/errno.h"

#define AUX_SEND_BUSY      (1u << 31)
#define AUX_DONE           (1u << 30)
#define AUX_TIME_OUT_ERROR (1u << 28)
#define AUX_TIME_OUT_MAX   (3u << 26)      /* 1600 us: the longest Gen9 offers */
#define AUX_RECEIVE_ERROR  (1u << 25)
#define AUX_MSG_SIZE_SHIFT 20
#define AUX_MSG_SIZE_MASK  (0x1fu << AUX_MSG_SIZE_SHIFT)
#define AUX_FW_SYNC(n)     (((n) - 1u) << 5)
#define AUX_SYNC(n)        ((n) - 1u)

#define AUX_IDLE_WAIT_US   10000u   /* i915's intel_dp_aux_wait_done */
#define AUX_TRIES          5u
#define AUX_DEFER_TRIES    32u      /* DP 1.4 asks for at least 7; Linux uses 32 */
#define AUX_DEFER_DELAY_US 500u

/* AUX request codes (DP 1.4 table 2-176), high nibble of byte 0. */
#define AUX_NATIVE_WRITE 0x8u
#define AUX_NATIVE_READ  0x9u
#define AUX_I2C_WRITE    0x0u
#define AUX_I2C_READ     0x1u
#define AUX_I2C_MOT      0x4u      /* middle of transaction: no stop after it */

#define AUX_REPLY_NATIVE(b) (((b) >> 4) & 3u)
#define AUX_REPLY_I2C(b)    (((b) >> 6) & 3u)
#define AUX_ACK   0u
#define AUX_NACK  1u
#define AUX_DEFER 2u

#define AUX_MAX_PAYLOAD 16u

static int wait_idle(const struct intel_regs *r, unsigned ch, uint32_t *status)
{
    uint64_t deadline = r->now_us(r->ctx) + AUX_IDLE_WAIT_US;
    for (;;) {
        uint32_t s = intel_rd(r, DP_AUX_CH_CTL(ch));
        if (!(s & AUX_SEND_BUSY)) { *status = s; return 0; }
        if (r->now_us(r->ctx) >= deadline) { *status = s; return -EMBK_EBUSY; }
    }
}

int intel_aux_transfer(const struct intel_regs *r, unsigned ch,
                       const uint8_t *tx, unsigned txlen,
                       uint8_t *rx, unsigned rxcap)
{
    if (ch >= INTEL_AUX_CHANNELS || !tx || txlen == 0 || txlen > 20)
        return -EMBK_EINVAL;

    uint32_t status;
    if (wait_idle(r, ch, &status))
        return -EMBK_EBUSY;           /* someone else's transaction never ended */

    for (unsigned i = 0; i < txlen; i += 4) {
        uint32_t word = 0;
        for (unsigned b = 0; b < 4 && i + b < txlen; b++)
            word |= (uint32_t)tx[i + b] << ((3u - b) * 8u);   /* byte 0 is MSB */
        intel_wr(r, DP_AUX_CH_DATA(ch, i / 4), word);
    }

    const uint32_t send = AUX_SEND_BUSY | AUX_DONE | AUX_TIME_OUT_ERROR |
                          AUX_TIME_OUT_MAX | AUX_RECEIVE_ERROR |
                          (txlen << AUX_MSG_SIZE_SHIFT) |
                          AUX_FW_SYNC(32u) | AUX_SYNC(32u);
    int done = 0, timed_out = 0;
    for (unsigned attempt = 0; attempt < AUX_TRIES && !done; attempt++) {
        intel_wr(r, DP_AUX_CH_CTL(ch), send);
        if (wait_idle(r, ch, &status))
            return -EMBK_EBUSY;       /* the controller hung mid-message */
        /* Status bits are write-one-to-clear; leave them clear for the next. */
        intel_wr(r, DP_AUX_CH_CTL(ch),
                 status | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_RECEIVE_ERROR);
        if (status & AUX_TIME_OUT_ERROR) { timed_out = 1; continue; }
        if (status & AUX_RECEIVE_ERROR) { intel_udelay(r, 500); continue; }
        done = (status & AUX_DONE) != 0;
    }
    if (!done)
        return timed_out ? -EMBK_ETIMEDOUT : -EMBK_EIO;

    unsigned got = (status & AUX_MSG_SIZE_MASK) >> AUX_MSG_SIZE_SHIFT;
    if (got == 0 || got > 20)
        return -EMBK_EIO;
    unsigned keep = got < rxcap ? got : rxcap;
    for (unsigned i = 0; i < keep; i += 4) {
        uint32_t word = intel_rd(r, DP_AUX_CH_DATA(ch, i / 4));
        for (unsigned b = 0; b < 4 && i + b < keep; b++)
            rx[i + b] = (uint8_t)(word >> ((3u - b) * 8u));
    }
    return (int)got;
}

static void header(uint8_t *tx, unsigned request, uint32_t addr)
{
    tx[0] = (uint8_t)((request << 4) | ((addr >> 16) & 0xfu));
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)addr;
}

int intel_dpcd_read(const struct intel_regs *r, unsigned ch, uint32_t addr,
                    uint8_t *buf, unsigned len)
{
    if (addr > 0xfffffu || len > 0x100000u - addr)
        return -EMBK_EINVAL;
    unsigned done = 0, defers = 0;
    while (done < len) {
        unsigned want = len - done < AUX_MAX_PAYLOAD ? len - done : AUX_MAX_PAYLOAD;
        uint8_t tx[4], rx[20];
        header(tx, AUX_NATIVE_READ, addr + done);
        tx[3] = (uint8_t)(want - 1u);
        int n = intel_aux_transfer(r, ch, tx, 4, rx, sizeof rx);
        if (n < 0) return n;
        switch (AUX_REPLY_NATIVE(rx[0])) {
        case AUX_ACK:
            /* A sink may return fewer bytes than asked; take what came and ask
             * again for the rest. A reply with nothing in it is not progress. */
            if (n < 2) return -EMBK_EIO;
            if ((unsigned)(n - 1) > want) n = (int)want + 1;
            for (int i = 1; i < n; i++) buf[done++] = rx[i];
            defers = 0;
            break;
        case AUX_DEFER:
            if (++defers > AUX_DEFER_TRIES) return -EMBK_ETIMEDOUT;
            intel_udelay(r, AUX_DEFER_DELAY_US);
            break;
        default:
            return -EMBK_EIO;
        }
    }
    return 0;
}

int intel_dpcd_write(const struct intel_regs *r, unsigned ch, uint32_t addr,
                     const uint8_t *buf, unsigned len)
{
    if (addr > 0xfffffu || len > 0x100000u - addr)
        return -EMBK_EINVAL;
    unsigned done = 0, defers = 0;
    while (done < len) {
        unsigned n = len - done < AUX_MAX_PAYLOAD ? len - done : AUX_MAX_PAYLOAD;
        uint8_t tx[4 + AUX_MAX_PAYLOAD], rx[20];
        header(tx, AUX_NATIVE_WRITE, addr + done);
        tx[3] = (uint8_t)(n - 1u);
        for (unsigned i = 0; i < n; i++) tx[4 + i] = buf[done + i];
        int got = intel_aux_transfer(r, ch, tx, 4 + n, rx, sizeof rx);
        if (got < 0) return got;
        switch (AUX_REPLY_NATIVE(rx[0])) {
        case AUX_ACK:   done += n; defers = 0; break;
        case AUX_DEFER:
            if (++defers > AUX_DEFER_TRIES) return -EMBK_ETIMEDOUT;
            intel_udelay(r, AUX_DEFER_DELAY_US);
            break;
        default:        return -EMBK_EIO;
        }
    }
    return 0;
}

/* One I2C-over-AUX message. len 0 is an address-only message (start or stop).
 * Returns the number of data bytes the sink returned (reads), or -errno. */
static int i2c_msg(const struct intel_regs *r, unsigned ch, unsigned request,
                   uint8_t i2c_addr, const uint8_t *wdata, uint8_t *rdata,
                   unsigned len)
{
    for (unsigned defers = 0; ; ) {
        uint8_t tx[4 + AUX_MAX_PAYLOAD], rx[20];
        unsigned txlen = 3;
        header(tx, request, i2c_addr);
        if (len) {
            tx[3] = (uint8_t)(len - 1u);
            txlen = 4;
            if (wdata) {
                for (unsigned i = 0; i < len; i++) tx[4 + i] = wdata[i];
                txlen += len;
            }
        }
        int n = intel_aux_transfer(r, ch, tx, txlen, rx, sizeof rx);
        if (n < 0) return n;
        unsigned native = AUX_REPLY_NATIVE(rx[0]), i2c = AUX_REPLY_I2C(rx[0]);
        if (native == AUX_ACK && i2c == AUX_ACK) {
            if (!rdata) return 0;
            unsigned got = (unsigned)n - 1u;
            if (got > len) got = len;
            for (unsigned i = 0; i < got; i++) rdata[i] = rx[1 + i];
            return (int)got;
        }
        if (native == AUX_DEFER || i2c == AUX_DEFER) {
            if (++defers > AUX_DEFER_TRIES) return -EMBK_ETIMEDOUT;
            intel_udelay(r, AUX_DEFER_DELAY_US);
            continue;
        }
        return -EMBK_EIO;                 /* NACK: no device at that address */
    }
}

int intel_aux_i2c_read(const struct intel_regs *r, unsigned ch, uint8_t i2c_addr,
                       uint8_t offset, uint8_t *buf, unsigned len)
{
    /* The shape Linux's drm_dp_i2c_xfer uses, which every sink accepts: a bare
     * start, the offset write, a bare restart into reading, the reads with
     * MOT held, and a bare message without MOT to stop. */
    int rc = i2c_msg(r, ch, AUX_I2C_WRITE | AUX_I2C_MOT, i2c_addr, 0, 0, 0);
    if (!rc) rc = i2c_msg(r, ch, AUX_I2C_WRITE | AUX_I2C_MOT, i2c_addr, &offset, 0, 1);
    if (!rc) rc = i2c_msg(r, ch, AUX_I2C_READ | AUX_I2C_MOT, i2c_addr, 0, 0, 0);
    unsigned done = 0, stalls = 0;
    while (!rc && done < len) {
        unsigned want = len - done < AUX_MAX_PAYLOAD ? len - done : AUX_MAX_PAYLOAD;
        int got = i2c_msg(r, ch, AUX_I2C_READ | AUX_I2C_MOT, i2c_addr, 0, buf + done, want);
        if (got < 0) { rc = got; break; }
        if (got == 0 && ++stalls > AUX_DEFER_TRIES) { rc = -EMBK_EIO; break; }
        done += (unsigned)got;
    }
    /* Always release the bus, even after a failure part way through. */
    int stop = i2c_msg(r, ch, AUX_I2C_READ, i2c_addr, 0, 0, 0);
    return rc ? rc : stop;
}
