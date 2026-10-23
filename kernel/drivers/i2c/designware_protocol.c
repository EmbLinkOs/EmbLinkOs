/* Synopsys DW_apb_i2c master, polling FIFO transport. DATA_CMD STOP/RESTART
 * preserve combined-transfer semantics. Bound read commands by RX FIFO room:
 * TX FIFO room alone does not prevent receive overflow. */
#include "designware.h"
#define RD(r) io->read(io->ctx, r)
#define WR(r,v) io->write(io->ctx, r, v)
static int enabled(struct dw_i2c_io *io, unsigned value, uint64_t start) {
    WR(0x6c, value);
    while ((RD(0x9c) & 1) != value) {
        if (io->clock_us(io->ctx)-start >= 1000000) return -1;
        io->pause(io->ctx);
    }
    return 0;
}
int dw_i2c_transfer(struct dw_i2c_io *io, uint16_t address,
    const uint8_t *write, unsigned wn, uint8_t *read, unsigned rn) {
    if (address < 8 || address > 0x77 || (!wn && !rn) || wn > 4096 || rn > 4096
        || (wn && !write) || (rn && !read) || !io->rx_depth) return -3;
    uint64_t start = io->clock_us(io->ctx);
    io->abort_source = 0;
    while (RD(0x70) & 0x20) {
        if (io->clock_us(io->ctx)-start >= 1000000) return -1;
        io->pause(io->ctx);
    }
    if (enabled(io, 0, start)) return -1;
    WR(0x04, address);
    (void)RD(0x40); /* CLR_INTR */
    if (enabled(io, 1, start)) return -1;
    unsigned sent = 0, requested = 0, received = 0;
    int result = 0, stopped = 0;
    for (;;) {
        uint32_t raw = RD(0x34);
        if (raw & (1 << 6)) { io->abort_source = RD(0x80); (void)RD(0x54); result = -2; break; }
        if (raw & (1 << 9)) { (void)RD(0x60); stopped = 1; }
        while ((RD(0x70) & 8) && received < rn) read[received++] = RD(0x10);
        if (stopped && received == rn && sent == wn && requested == rn) break;
        if (RD(0x70) & 2) { /* TFNF */
            if (sent < wn) {
                unsigned v = write[sent++];
                if (sent == wn && !rn) v |= 1 << 9;
                WR(0x10, v);
            } else if (requested < rn && requested-received < io->rx_depth) {
                unsigned v = 1 << 8;
                if (!requested && wn) v |= 1 << 10;
                if (++requested == rn) v |= 1 << 9;
                WR(0x10, v);
            }
        }
        if (io->clock_us(io->ctx)-start >= 1000000) { result = -1; break; }
        io->pause(io->ctx);
    }
    if (result) {
        /* ABORT drains pending commands before disabling/reprogramming TAR. */
        WR(0x6c, 3);
        uint64_t abort_start = io->clock_us(io->ctx);
        while (RD(0x6c) & 2) {
            if (io->clock_us(io->ctx)-abort_start >= 10000) break;
            io->pause(io->ctx);
        }
    }
    if (enabled(io, 0, io->clock_us(io->ctx)) && !result) result = -1;
    return result;
}
