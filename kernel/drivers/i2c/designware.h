#ifndef EMBLINK_DESIGNWARE_I2C_H
#define EMBLINK_DESIGNWARE_I2C_H
#include <stdint.h>
struct dw_i2c_io {
    void *ctx;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    uint64_t (*clock_us)(void *);
    void (*pause)(void *);
    unsigned rx_depth;
    uint32_t abort_source;
};
/* Returns 0, -1 timeout, -2 NACK/abort, -3 invalid. Caller holds bus mutex. */
int dw_i2c_transfer(struct dw_i2c_io *, uint16_t address,
    const uint8_t *write, unsigned write_len, uint8_t *read, unsigned read_len);
struct dw_i2c_bus;
void dw_i2c_init(void);
struct dw_i2c_bus *dw_i2c_find(unsigned bus, unsigned device, unsigned function);
int dw_i2c_xfer(struct dw_i2c_bus *, uint16_t address, uint32_t speed,
    const uint8_t *write, unsigned write_len, uint8_t *read, unsigned read_len);
#endif
