/* Firmware-backed power information. No fabricated values on missing hardware. */
#include "acpi/runtime.h"
#include "acpi/power.h"
#include "acpi/display.h"
#include "acpi/ec.h"
#include "drivers/i2c/hid_i2c.h"
#include "include/kprintf.h"
#include <uacpi/uacpi.h>
#include <uacpi/context.h>
#include <uacpi/utilities.h>
#include <uacpi/namespace.h>

static int ready;
extern int acpi_host_start(void);
void acpi_runtime_init(void) {
    if (!acpi_host_start()) { kprintf("ACPI: no firmware worker\n"); return; }
    uacpi_context_set_loop_timeout(5);
    uacpi_context_set_max_call_stack_depth(64);
    uacpi_status s = uacpi_initialize(UACPI_FLAG_BAD_CSUM_FATAL);
    if (s == UACPI_STATUS_OK) {
        acpi_ec_early_init();
        s = uacpi_namespace_load();
    }
    if (s == UACPI_STATUS_OK) acpi_ec_discover();
    if (s == UACPI_STATUS_OK) s = uacpi_namespace_initialize();
    if (s != UACPI_STATUS_OK) {
        kprintf("ACPI: firmware initialization failed: %s\n", uacpi_status_to_string(s));
        return;
    }
    acpi_ec_enable_events();
    i2c_hid_init();
    ready = 1;
    acpi_power_init();
    kprintf("ACPI: AML namespace initialized\n");
}
static int package_integers(uacpi_namespace_node *node, const char *name,
                            uint64_t *out, unsigned count) {
    uacpi_object *obj = NULL;
    if (uacpi_eval_simple_package(node, name, &obj) != UACPI_STATUS_OK) return 0;
    uacpi_object_array a;
    int ok = uacpi_object_get_package(obj, &a) == UACPI_STATUS_OK && a.count >= count;
    for (unsigned i = 0; ok && i < count; i++)
        ok = uacpi_object_get_integer(a.objects[i], &out[i]) == UACPI_STATUS_OK;
    uacpi_object_unref(obj);
    return ok;
}
struct report { unsigned batteries, adapters, errors; };
static uacpi_iteration_decision battery(void *ctx, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    struct report *r = ctx;
    unsigned index = r->batteries++;
    uint64_t sta;
    if (uacpi_eval_simple_integer(node, "_STA", &sta) != UACPI_STATUS_OK) sta = 0x1f;
    if (!(sta & 0x10)) { kprintf("battery%u: not installed\n", index); return UACPI_ITERATION_DECISION_CONTINUE; }
    uint64_t bst[4], info[4];
    if (!package_integers(node, "_BST", bst, 4)) {
        kprintf("battery%u: _BST unavailable\n", index); r->errors++;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    int bix = package_integers(node, "_BIX", info, 4);
    int has_info = bix || package_integers(node, "_BIF", info, 3);
    uint64_t unit = has_info ? info[bix ? 1 : 0] : 0xffffffff;
    uint64_t full = has_info ? info[bix ? 3 : 2] : 0xffffffff;
    const char *energy = unit == 0 ? "mWh" : unit == 1 ? "mAh" : "unknown units";
    const char *rate = unit == 0 ? "mW" : unit == 1 ? "mA" : "unknown units";
    kprintf("battery%u: state=0x%llx (%s%s%s)\n", index, bst[0],
        bst[0] & 1 ? "discharging " : "", bst[0] & 2 ? "charging " : "",
        bst[0] & 4 ? "critical" : "");
    if (bst[2] != 0xffffffff && bst[2] <= UINT32_MAX) {
        kprintf("  remaining=%llu %s", bst[2], energy);
        if (full && full <= UINT32_MAX && full != 0xffffffff) {
            uint64_t pct = bst[2] * 100 / full;
            kprintf(" (%llu%%)", pct > 100 ? 100 : pct);
        }
        kprintf("\n");
    } else kprintf("  remaining=unknown\n");
    if (bst[1] != 0xffffffff) kprintf("  rate=%llu %s\n", bst[1], rate);
    if (bst[3] != 0xffffffff) kprintf("  voltage=%llu mV\n", bst[3]);
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_iteration_decision adapter(void *ctx, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    struct report *r = ctx; uint64_t online;
    unsigned index = r->adapters++;
    if (uacpi_eval_simple_integer(node, "_PSR", &online) == UACPI_STATUS_OK)
        kprintf("ac%u: %s\n", index, online ? "online" : "offline");
    else { kprintf("ac%u: status unavailable\n", index); r->errors++; }
    return UACPI_ITERATION_DECISION_CONTINUE;
}
int acpi_power_report(void) {
    if (!ready) { kprintf("ACPI: firmware runtime unavailable\n"); return 1; }
    struct report r = {0};
    uacpi_status b = uacpi_find_devices("PNP0C0A", battery, &r);
    uacpi_status a = uacpi_find_devices("ACPI0003", adapter, &r);
    if (!r.batteries) kprintf("ACPI: no battery reported by firmware\n");
    if (!r.adapters) kprintf("ACPI: no AC adapter reported by firmware\n");
    return r.errors || b != UACPI_STATUS_OK || a != UACPI_STATUS_OK;
}

int acpi_runtime_ready(void) { return ready; }
