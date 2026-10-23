/* ACPI Embedded Controller: firmware resources, operation regions and GPE
 * query dispatch. No port probing: ECDT or PNP0C09 _CRS supplies both ports. */
#include "acpi/ec.h"
#include "drivers/platform/ec_protocol.h"
#include "include/io.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "process/ksync.h"
#include "drivers/timer/timer.h"
#include <uacpi/uacpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/opregion.h>
#include <uacpi/event.h>
#include <uacpi/tables.h>
/* Pinned uACPI 7: only inspect AML global-lock ownership. Acquiring the public
 * global lock recursively would deadlock an AML Field(..., Lock, ...) access. */
#include <uacpi/internal/context.h>
#include <uacpi/internal/mutex.h>

#define MAX_ECS 2
struct ec_device {
    struct ec_transport bus;
    struct mutex lock;
    uint16_t data, command, gpe;
    int used, global_lock, region_installed, event_installed, event_enabled;
    uacpi_namespace_node *node;
    uacpi_handle work;
};
static struct ec_device ecs[MAX_ECS];
static uint8_t status(void *p) { return inb(((struct ec_device *)p)->command); }
static uint8_t read_data(void *p) { return inb(((struct ec_device *)p)->data); }
static void command(void *p, uint8_t v) { outb(((struct ec_device *)p)->command, v); }
static void write_data(void *p, uint8_t v) { outb(((struct ec_device *)p)->data, v); }
static uint64_t clock_us(void *p) { (void)p; return time_get_us(); }
static void pause_ec(void *p) { (void)p; __asm__ volatile("pause"); }
static struct ec_device *get_ec(uint16_t data, uint16_t cmd) {
    if (!data || !cmd || data == cmd) return NULL;
    for (unsigned i = 0; i < MAX_ECS; i++)
        if (ecs[i].used && ecs[i].data == data && ecs[i].command == cmd) return &ecs[i];
    for (unsigned i = 0; i < MAX_ECS; i++) if (!ecs[i].used) {
        struct ec_device *e = &ecs[i];
        e->data = data; e->command = cmd; e->gpe = 0xffff; e->used = 1;
        mutex_init(&e->lock);
        e->bus = (struct ec_transport){ .ctx=e, .status=status, .read_data=read_data,
            .write_command=command, .write_data=write_data, .clock_us=clock_us, .pause=pause_ec };
        return e;
    }
    return NULL;
}
/* Always acquire the firmware lock before the EC mutex. AML may already own
 * it; in that case it remains owned and must not be released here. */
static uacpi_status lock_ec(struct ec_device *e, uint32_t *seq, int *acquired) {
    *acquired = 0;
    if (e->global_lock && !uacpi_this_thread_owns_aml_mutex(g_uacpi_rt_ctx.global_lock_mutex)) {
        uacpi_status s = uacpi_acquire_global_lock(1000, seq);
        if (s != UACPI_STATUS_OK) return s;
        *acquired = 1;
    }
    mutex_lock(&e->lock);
    return UACPI_STATUS_OK;
}
static void unlock_ec(struct ec_device *e, uint32_t seq, int acquired) {
    mutex_unlock(&e->lock);
    if (acquired) uacpi_release_global_lock(seq);
}
static uacpi_status region(uacpi_region_op op, uacpi_handle data) {
    if (op == UACPI_REGION_OP_ATTACH) {
        uacpi_region_attach_data *a = data;
        if (a->generic_info.base >= 256 || a->generic_info.length > 256 - a->generic_info.base)
            return UACPI_STATUS_INVALID_ARGUMENT;
        a->out_region_context = a->handler_context;
        return UACPI_STATUS_OK;
    }
    if (op == UACPI_REGION_OP_DETACH) return UACPI_STATUS_OK;
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE) return UACPI_STATUS_UNIMPLEMENTED;
    uacpi_region_rw_data *r = data;
    struct ec_device *e = r->handler_context;
    if (!r->byte_width || r->byte_width > 8 || r->address >= 256 || r->byte_width > 256-r->address)
        return UACPI_STATUS_INVALID_ARGUMENT;
    uint32_t seq = 0; int acquired;
    uacpi_status s = lock_ec(e, &seq, &acquired);
    if (s != UACPI_STATUS_OK) return s;
    uint64_t value = op == UACPI_REGION_OP_WRITE ? r->value : 0;
    int rc = 0;
    for (unsigned i = 0; i < r->byte_width && !rc; i++) {
        uint8_t b = 0;
        if (op == UACPI_REGION_OP_WRITE) rc = ec_write_byte(&e->bus, r->address+i, value >> (8*i));
        else { rc = ec_read_byte(&e->bus, r->address+i, &b); value |= (uint64_t)b << (8*i); }
    }
    unlock_ec(e, seq, acquired);
    if (rc) return UACPI_STATUS_HARDWARE_TIMEOUT;
    r->value = value;
    return UACPI_STATUS_OK;
}
void acpi_ec_early_init(void) {
    uacpi_table table;
    if (uacpi_table_find_by_signature("ECDT", &table) != UACPI_STATUS_OK) return;
    struct acpi_ecdt *t = table.ptr;
    if (t->hdr.length >= sizeof(*t) && t->ec_control.address_space_id == 1
        && t->ec_data.address_space_id == 1 && t->ec_control.address <= 0xffff
        && t->ec_data.address <= 0xffff && !t->ec_control.register_bit_offset
        && !t->ec_data.register_bit_offset && t->ec_control.register_bit_width == 8
        && t->ec_data.register_bit_width == 8) {
        struct ec_device *e = get_ec(t->ec_data.address, t->ec_control.address);
        if (e) {
            e->global_lock = 1; e->gpe = t->gpe_bit;
            e->region_installed = uacpi_install_address_space_handler(uacpi_namespace_root(),
                UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, region, e) == UACPI_STATUS_OK;
            kprintf("EC: ECDT ports %x/%x, early handler %s\n", e->data, e->command,
                e->region_installed ? "ready" : "failed");
        }
    }
    uacpi_table_unref(&table);
}
struct ports { uint16_t data, command; unsigned count; };
static uacpi_iteration_decision resource(void *ctx, uacpi_resource *r) {
    struct ports *p = ctx; uint16_t port = 0;
    if (r->type == UACPI_RESOURCE_TYPE_IO && r->io.length == 1 && r->io.minimum == r->io.maximum)
        port = r->io.minimum;
    if (r->type == UACPI_RESOURCE_TYPE_FIXED_IO && r->fixed_io.length == 1) port = r->fixed_io.address;
    if (port) { if (!p->count) p->data = port; else if (p->count == 1) p->command = port; p->count++; }
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static void queries(uacpi_handle ctx) {
    struct ec_device *e = ctx;
    for (unsigned i = 0; i < 32; i++) {
        uint32_t seq = 0; int acquired; uint8_t query = 0;
        uacpi_status s = lock_ec(e, &seq, &acquired);
        if (s != UACPI_STATUS_OK) { kprintf("EC: global lock unavailable; GPE left disabled\n"); return; }
        int pending = status(e) & 0x20;
        int rc = pending ? ec_query(&e->bus, &query) : 0;
        unlock_ec(e, seq, acquired);
        if (rc) { kprintf("EC: query timeout; GPE left disabled\n"); return; }
        if (!pending || !query) {
            uacpi_finish_handling_gpe(NULL, e->gpe); return;
        }
        char method[] = "_Q00";
        const char *hex = "0123456789ABCDEF";
        method[2] = hex[query >> 4]; method[3] = hex[query & 15];
        s = uacpi_execute(e->node, method, NULL);
        if (s != UACPI_STATUS_OK && s != UACPI_STATUS_NOT_FOUND)
            kprintf("EC: %s failed: %s\n", method, uacpi_status_to_string(s));
    }
    /* Bound one batch, without discarding remaining firmware notifications. */
    uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, e->work, queries, e);
}
static uacpi_interrupt_ret gpe(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u16 idx) {
    (void)node; (void)idx;
    struct ec_device *e = ctx;
    uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, e->work, queries, e);
    return UACPI_INTERRUPT_HANDLED; /* finish_handling_gpe after deferred query */
}
static uacpi_iteration_decision discover(void *ctx, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)ctx; (void)depth;
    struct ports p = {0};
    if (uacpi_for_each_device_resource(node, "_CRS", resource, &p) != UACPI_STATUS_OK || p.count != 2)
        return UACPI_ITERATION_DECISION_CONTINUE;
    struct ec_device *e = get_ec(p.data, p.command);
    if (!e || e->node) return UACPI_ITERATION_DECISION_CONTINUE;
    e->node = node;
    uint64_t v;
    uacpi_status glk=uacpi_eval_simple_integer(node, "_GLK", &v);
    if (glk == UACPI_STATUS_OK) e->global_lock = !!v;
    else if (glk == UACPI_STATUS_NOT_FOUND) e->global_lock = 0;
    else {
        kprintf("EC: _GLK evaluation failed; controller unavailable\n");
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (uacpi_eval_simple_integer(node, "_GPE", &v) == UACPI_STATUS_OK && v <= 0xffff) e->gpe = v;
    /* Install a device-specific handler even when ECDT supplied a root handler. */
    uacpi_status s = uacpi_install_address_space_handler(node,
        UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, region, e);
    if (s == UACPI_STATUS_OK) s = uacpi_reg_all_opregions(node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER);
    if (s != UACPI_STATUS_OK) { kprintf("EC: operation-region setup failed: %s\n", uacpi_status_to_string(s)); return UACPI_ITERATION_DECISION_CONTINUE; }
    e->region_installed = 1;
    if (e->gpe != 0xffff) {
        e->work = uacpi_kernel_create_work_item();
        if (e->work && uacpi_install_gpe_handler(NULL, e->gpe, UACPI_GPE_TRIGGERING_LEVEL, gpe, e) == UACPI_STATUS_OK)
            e->event_installed = 1;
        else { uacpi_kernel_free_work_item(e->work); e->work = NULL; }
    }
    kprintf("EC: ports %x/%x, global lock %u, query events %s\n", e->data, e->command,
            e->global_lock, e->event_installed ? "ready" : "unavailable");
    return UACPI_ITERATION_DECISION_CONTINUE;
}
void acpi_ec_discover(void) { uacpi_find_devices("PNP0C09", discover, NULL); }
void acpi_ec_enable_events(void) {
    for (unsigned i = 0; i < MAX_ECS; i++) {
        struct ec_device *e = &ecs[i];
        if (e->event_installed && !e->event_enabled) {
            if (uacpi_enable_gpe(NULL, e->gpe) == UACPI_STATUS_OK) e->event_enabled = 1;
        }
    }
}
