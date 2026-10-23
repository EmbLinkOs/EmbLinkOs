#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/platform/ec_protocol.h"
struct model {
    uint8_t memory[256], command, address, output, query;
    unsigned phase, busy, obf, never_ready, no_response, commands, writes;
    uint64_t now;
};
static uint8_t status(void *ctx) {
    struct model *m = ctx;
    if (m->never_ready) return 2;
    if (m->busy) { m->busy--; return 2; }
    return m->obf ? 1 : 0;
}
static uint8_t read_data(void *ctx) {
    struct model *m = ctx;
    assert(m->obf && !m->busy); m->obf = 0;
    return m->output;
}
static void command(void *ctx, uint8_t v) {
    struct model *m = ctx;
    assert(!m->busy && !m->obf);
    m->commands++; m->command = v; m->phase = 0; m->busy = 2;
    assert(v == 0x80 || v == 0x81 || v == 0x84);
    if (v == 0x84 && !m->no_response) { m->output = m->query; m->obf = 1; }
}
static void write_data(void *ctx, uint8_t v) {
    struct model *m = ctx;
    assert(!m->busy && !m->obf);
    m->writes++; m->busy = 2;
    if (!m->phase++) {
        m->address = v;
        if (m->command == 0x80 && !m->no_response) { m->output = m->memory[v]; m->obf = 1; }
    } else { assert(m->command == 0x81 && m->phase == 2); m->memory[m->address] = v; }
}
static uint64_t clock_us(void *ctx) { return ((struct model *)ctx)->now; }
static void pause_ec(void *ctx) { ((struct model *)ctx)->now += 1000; }
static struct ec_transport transport(struct model *m) {
    return (struct ec_transport){m, status, read_data, command, write_data, clock_us, pause_ec, 0};
}
int main(void) {
    struct model m = {0}; struct ec_transport t = transport(&m); uint8_t v = 0;
    for (unsigned i = 0; i < 256; i++) {
        assert(ec_write_byte(&t, i, i ^ 0x5a) == EC_OK);
        assert(ec_read_byte(&t, i, &v) == EC_OK && v == (i ^ 0x5a));
    }
    m.query = 0xa7; assert(ec_query(&t, &v) == EC_OK && v == 0xa7);
    assert(ec_read_byte(&t, 0, NULL) == EC_INVALID);
    m.never_ready = 1;
    unsigned commands = m.commands;
    assert(ec_read_byte(&t, 1, &v) == EC_TIMEOUT && t.failed);
    assert(m.commands == commands && m.now >= 500000);
    m.never_ready = 0;
    assert(ec_write_byte(&t, 1, 99) == EC_DESYNC && m.commands == commands);
    memset(&m, 0, sizeof m); t = transport(&m); m.no_response = 1;
    assert(ec_read_byte(&t, 1, &v) == EC_TIMEOUT && t.failed);
    memset(&m, 0, sizeof m); t = transport(&m); m.obf = 1;
    assert(ec_query(&t, &v) == EC_DESYNC && t.failed && !m.commands);
    puts("EC protocol: all address values, handshakes, query, timeout and desync checks passed");
}
