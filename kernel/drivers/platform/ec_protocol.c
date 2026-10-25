/* ACPI 6.6 section 12: IBF/OBF handshakes, RD_EC/WR_EC/QR_EC.
 * A timed-out transaction must not be retried as a new command: a late EC byte
 * could otherwise be mistaken for the next battery reading or write address. */
#include "ec_protocol.h"
#define OBF 1
#define IBF 2
#define TIMEOUT_US 500000ULL
static int wait_status(struct ec_transport *t, unsigned mask, unsigned wanted,
                       uint64_t start) {
    for (;;) {
        if ((t->status(t->ctx) & mask) == wanted) return EC_OK;
        if (t->clock_us(t->ctx) - start >= TIMEOUT_US) {
            t->failed = 1; return EC_TIMEOUT;
        }
        t->pause(t->ctx);
    }
}
static int begin(struct ec_transport *t, uint8_t cmd, uint64_t start) {
    if (t->failed) return EC_DESYNC;
    int rc = wait_status(t, IBF, 0, start);
    if (rc) return rc;
    if (t->status(t->ctx) & OBF) { t->failed = 1; return EC_DESYNC; }
    t->write_command(t->ctx, cmd);
    return wait_status(t, IBF, 0, start);
}
int ec_read_byte(struct ec_transport *t, uint8_t address, uint8_t *out) {
    if (!out) return EC_INVALID;
    uint64_t start = t->clock_us(t->ctx);
    int rc = begin(t, 0x80, start);
    if (rc) return rc;
    t->write_data(t->ctx, address);
    rc = wait_status(t, OBF, OBF, start);
    if (!rc) *out = t->read_data(t->ctx);
    return rc;
}
int ec_write_byte(struct ec_transport *t, uint8_t address, uint8_t value) {
    uint64_t start = t->clock_us(t->ctx);
    int rc = begin(t, 0x81, start);
    if (rc) return rc;
    t->write_data(t->ctx, address);
    rc = wait_status(t, IBF, 0, start);
    if (rc) return rc;
    t->write_data(t->ctx, value);
    return wait_status(t, IBF, 0, start);
}
int ec_query(struct ec_transport *t, uint8_t *out) {
    if (!out) return EC_INVALID;
    uint64_t start = t->clock_us(t->ctx);
    int rc = begin(t, 0x84, start);
    if (!rc) rc = wait_status(t, OBF, OBF, start);
    if (!rc) *out = t->read_data(t->ctx);
    return rc;
}
