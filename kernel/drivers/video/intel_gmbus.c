/* intel_gmbus.c -- polled GMBUS reads, the sequence Linux's i915 uses
 * (intel_gmbus.c: gmbus_xfer, gmbus_xfer_read_chunk, gmbus_index_xfer). */
#include "drivers/video/intel_gmbus.h"
#include "include/errno.h"

#define GMBUS0 0xc5100u              /* clock rate, pin pair */
#define GMBUS1 0xc5104u              /* command */
#define GMBUS2 0xc5108u              /* status */
#define GMBUS3 0xc510cu              /* data FIFO: reading it consumes bytes */
#define GMBUS4 0xc5110u              /* interrupt mask */

#define GMBUS_RATE_100KHZ    (0u << 8)
#define GMBUS_SW_CLR_INT     (1u << 31)
#define GMBUS_SW_RDY         (1u << 30)
#define GMBUS_CYCLE_WAIT     (1u << 25)
#define GMBUS_CYCLE_INDEX    (2u << 25)
#define GMBUS_CYCLE_STOP     (4u << 25)
#define GMBUS_BYTE_COUNT_SHIFT 16
#define GMBUS_INDEX_SHIFT    8
#define GMBUS_ADDR_SHIFT     1
#define GMBUS_READ           1u

#define GMBUS_HW_WAIT_PHASE  (1u << 14)
#define GMBUS_HW_RDY         (1u << 11)
#define GMBUS_SATOER         (1u << 10)  /* the slave did not acknowledge */
#define GMBUS_ACTIVE         (1u << 9)

#define GMBUS_WAIT_US 50000u            /* i915's gmbus_wait: 50 ms */

static int wait_for(const struct intel_regs *r, uint32_t bit)
{
    uint64_t deadline = r->now_us(r->ctx) + GMBUS_WAIT_US;
    for (;;) {
        uint32_t s = intel_rd(r, GMBUS2);
        if (s & GMBUS_SATOER) return -EMBK_ENXIO;
        if (s & bit) return 0;
        if (r->now_us(r->ctx) >= deadline) return -EMBK_ETIMEDOUT;
    }
}

static int wait_idle(const struct intel_regs *r)
{
    uint64_t deadline = r->now_us(r->ctx) + GMBUS_WAIT_US;
    while (intel_rd(r, GMBUS2) & GMBUS_ACTIVE)
        if (r->now_us(r->ctx) >= deadline) return -EMBK_ETIMEDOUT;
    return 0;
}

int intel_gmbus_read(const struct intel_regs *r, unsigned pin, uint8_t i2c_addr,
                     uint8_t offset, uint8_t *buf, unsigned len)
{
    if (!buf || len == 0 || len > GMBUS_MAX_READ || pin == 0 || pin > 7 || i2c_addr > 0x7f)
        return -EMBK_EINVAL;

    intel_wr(r, GMBUS4, 0);                           /* polled: no interrupts */
    intel_wr(r, GMBUS0, GMBUS_RATE_100KHZ | pin);
    /* An indexed read: the engine writes `offset` to the device, restarts and
     * reads, in one command -- what an EDID read is. */
    intel_wr(r, GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_INDEX | GMBUS_CYCLE_WAIT |
                        ((uint32_t)len << GMBUS_BYTE_COUNT_SHIFT) |
                        ((uint32_t)offset << GMBUS_INDEX_SHIFT) |
                        ((uint32_t)i2c_addr << GMBUS_ADDR_SHIFT) | GMBUS_READ);
    int rc = 0;
    unsigned done = 0;
    while (done < len) {
        rc = wait_for(r, GMBUS_HW_RDY);
        if (rc) break;
        uint32_t word = intel_rd(r, GMBUS3);
        for (unsigned b = 0; b < 4 && done < len; b++, word >>= 8)
            buf[done++] = (uint8_t)word;
    }
    if (!rc) rc = wait_for(r, GMBUS_HW_WAIT_PHASE);

    if (rc == 0) {
        intel_wr(r, GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_STOP);
        rc = wait_idle(r);
    } else {
        /* NAK or stall: let the bus go idle, clear the error, then release. */
        (void)wait_idle(r);
        intel_wr(r, GMBUS1, GMBUS_SW_CLR_INT);
        intel_wr(r, GMBUS1, 0);
    }
    intel_wr(r, GMBUS0, 0);
    return rc;
}
