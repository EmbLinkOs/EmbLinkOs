#ifndef EMBLINK_EC_PROTOCOL_H
#define EMBLINK_EC_PROTOCOL_H
#include <stdint.h>
/* ACPI EC byte protocol. Caller serializes entire transactions, including any
 * firmware global lock. Hooks make exactly the same transport testable. */
struct ec_transport {
    void *ctx;
    uint8_t (*status)(void *);
    uint8_t (*read_data)(void *);
    void (*write_command)(void *, uint8_t);
    void (*write_data)(void *, uint8_t);
    uint64_t (*clock_us)(void *);
    void (*pause)(void *);
    int failed;
};
#define EC_OK 0
#define EC_TIMEOUT (-1)
#define EC_INVALID (-2)
#define EC_DESYNC (-3)
int ec_read_byte(struct ec_transport *, uint8_t address, uint8_t *out);
int ec_write_byte(struct ec_transport *, uint8_t address, uint8_t value);
int ec_query(struct ec_transport *, uint8_t *out);
#endif
