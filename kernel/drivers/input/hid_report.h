#ifndef EMBLINK_HID_REPORT_H
#define EMBLINK_HID_REPORT_H
#include <stdint.h>
#define HID_FIELDS 256
#define HID_REPORTS 16
struct hid_field {
    uint16_t bit, count, page, usage, usage_max;
    uint8_t size, report, flags, application;
    int32_t minimum, maximum;
};
struct hid_report_layout {
    struct hid_field fields[HID_FIELDS];
    uint16_t bits[HID_REPORTS];
    uint8_t ids[HID_REPORTS];
    unsigned field_count, report_count, numbered;
};
struct hid_input_state {
    uint8_t report_keys[HID_REPORTS][32], keys[32];
    uint32_t report_buttons[HID_REPORTS];
};
struct hid_pointer {
    int32_t x, y, wheel;
    uint32_t buttons;
    int absolute, touchpad, touching;
};
struct hid_input_sink {
    void *ctx;
    void (*key)(void *, uint16_t usage, int down);
    void (*pointer)(void *, const struct hid_pointer *);
};
/* HID 1.11 short-item report descriptors. Bounds/error checks are shared by
 * host tests and real I2C input. Unsupported layouts fail rather than guessing. */
int hid_report_parse(struct hid_report_layout *, const uint8_t *, unsigned length);
int hid_report_decode(const struct hid_report_layout *, struct hid_input_state *,
    const uint8_t *report, unsigned length, const struct hid_input_sink *);
#endif
