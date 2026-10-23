#ifndef EMBLINK_I2C_HID_PROTOCOL_H
#define EMBLINK_I2C_HID_PROTOCOL_H
#include <stdint.h>
struct i2c_hid_transport {
    void *ctx;
    int (*xfer)(void *, const uint8_t *, unsigned, uint8_t *, unsigned);
    uint64_t (*clock_ms)(void *);
    void (*sleep_ms)(void *, unsigned);
};
struct i2c_hid_descriptor {
    uint16_t report_length, report_register, input_register, input_max;
    uint16_t command_register, vendor, product;
};
int i2c_hid_start(struct i2c_hid_transport *, uint16_t descriptor_register,
    struct i2c_hid_descriptor *, uint8_t *report_descriptor, unsigned capacity);
/* Input read has no register-address prefix (HID over I2C 1.0 input protocol).
 * 0=no data, positive=payload length, -4=unexpected device reset. */
int i2c_hid_read(struct i2c_hid_transport *, const struct i2c_hid_descriptor *,
    uint8_t *buffer, unsigned capacity);
#endif
