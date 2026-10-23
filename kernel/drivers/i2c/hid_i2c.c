/* ACPI PNP0C50/ACPI0C50 HID over Intel LPSS I2C. Standard report descriptors,
 * no device-specific boot-report assumptions. Polling at 100Hz avoids guessing
 * GPIO interrupt-controller routing; GPIO interrupt/power optimizations follow. */
#include "drivers/i2c/hid_i2c.h"
#include "drivers/i2c/hid_protocol.h"
#include "drivers/i2c/designware.h"
#include "drivers/input/hid_report.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "drivers/timer/timer.h"
#include "drivers/bus/pci.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "process/process.h"
#include <uacpi/uacpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/resources.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>
#define MAX_INPUTS 8
struct input {
    struct dw_i2c_bus *bus;
    struct i2c_hid_transport transport;
    struct i2c_hid_descriptor descriptor;
    struct hid_report_layout layout;
    struct hid_input_state state;
    uacpi_namespace_node *node;
    uint16_t address;
    uint32_t speed;
    uint8_t report_descriptor[4096],packet[1024];
    int online,anchored;
    int32_t x,y;
};
static struct input inputs[MAX_INPUTS];
static unsigned input_count;
static struct wait_queue input_wait;
static int running;
static struct dw_i2c_bus *pci_controller(uacpi_namespace_node *node) {
    uacpi_namespace_node *path[16];unsigned depth=0,bus=0;
    for(uacpi_namespace_node *n=node;n;n=uacpi_namespace_node_parent(n)) {
        if(depth==16) return NULL;
        path[depth++]=n;
    }
    while(depth) {
        uacpi_namespace_node *n=path[--depth];uint64_t value;
        if(uacpi_eval_simple_integer(n,"_SEG",&value)==UACPI_STATUS_OK && value) return NULL;
        if(uacpi_eval_simple_integer(n,"_BBN",&value)==UACPI_STATUS_OK) {
            if(value>255) return NULL;
            bus=value;
        }
        uacpi_id_string *hid=NULL;
        int root=0;
        if(uacpi_eval_hid(n,&hid)==UACPI_STATUS_OK) {
            root=!strcmp(hid->value,"PNP0A03") || !strcmp(hid->value,"PNP0A08");
            uacpi_free_id_string(hid);
        }
        if(root) continue; /* Root _ADR, when present, is not a PCI bridge. */
        if(uacpi_eval_simple_integer(n,"_ADR",&value)!=UACPI_STATUS_OK) continue;
        if(value>0x1f0007 || (value&0xffff)>7 || (value>>16)>31) return NULL;
        unsigned device=value>>16,function=value&7;
        if(n==node) return dw_i2c_find(bus,device,function);
        if(pci_read8(bus,device,function,0x0b)!=6 || pci_read8(bus,device,function,0x0a)!=4) return NULL;
        bus=pci_read8(bus,device,function,0x19);
    }
    return NULL;
}
struct connection {uacpi_namespace_node *node;struct dw_i2c_bus *bus;uint16_t address;uint32_t speed;unsigned count;};
static uacpi_iteration_decision resource(void *ctx,uacpi_resource *r) {
    struct connection *c=ctx;
    if(r->type!=UACPI_RESOURCE_TYPE_SERIAL_I2C_CONNECTION) return UACPI_ITERATION_DECISION_CONTINUE;
    c->count++;
    uacpi_resource_i2c_connection *i=&r->i2c_connection;
    if(i->addressing_mode!=UACPI_I2C_7BIT || i->common.mode!=UACPI_MODE_CONTROLLER_INITIATED
        || i->slave_address<8 || i->slave_address>0x77 || !i->common.source.string
        || !i->common.source.length || i->common.source.length>256
        || i->common.source.string[i->common.source.length-1]) return UACPI_ITERATION_DECISION_CONTINUE;
    uacpi_namespace_node *controller;
    if(uacpi_namespace_node_resolve_from_aml_namepath(c->node,i->common.source.string,&controller)!=UACPI_STATUS_OK)
        return UACPI_ITERATION_DECISION_CONTINUE;
    c->bus=pci_controller(controller);c->address=i->slave_address;c->speed=i->connection_speed;
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static int descriptor_register(uacpi_namespace_node *node,uint16_t *out) {
    uint8_t guid[]={0xf7,0xf6,0xdf,0x3c,0x67,0x42,0x55,0x45,0xad,0x05,0xb3,0x0a,0x3d,0x89,0x38,0xde};
    uacpi_object *args[4]={uacpi_object_create_buffer((uacpi_data_view){.bytes=guid,.length=16}),
        uacpi_object_create_integer(1),uacpi_object_create_integer(1),
        uacpi_object_create_package((uacpi_object_array){0})};
    int ok=args[0] && args[1] && args[2] && args[3];uint64_t value=0;
    if(ok) ok=uacpi_eval_integer(node,"_DSM",&(uacpi_object_array){args,4},&value)==UACPI_STATUS_OK && value<=0xffff;
    for(unsigned i=0;i<4;i++) uacpi_object_unref(args[i]);
    if(ok) *out=value;
    return ok;
}
static int xfer(void *ctx,const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn) {
    struct input *i=ctx;return dw_i2c_xfer(i->bus,i->address,i->speed,w,wn,r,rn);
}
static uint64_t clock_ms(void *ctx) {(void)ctx;return time_get_us()/1000;}
static void sleep_ms(void *ctx,unsigned ms) {(void)ctx;uacpi_kernel_sleep(ms);}
static void key(void *ctx,uint16_t usage,int down) {(void)ctx;keyboard_inject_hid_usage(usage,down);}
static void pointer(void *ctx,const struct hid_pointer *p) {
    struct input *i=ctx;
    if(p->absolute && p->touchpad) {
        int32_t dx=0,dy=0;
        if(p->touching && i->anchored) {dx=(p->x-i->x)/64;dy=(p->y-i->y)/64;}
        i->anchored=p->touching;i->x=p->x;i->y=p->y;
        mouse_inject_relative(dx,dy,p->buttons,p->wheel);
    } else if(p->absolute) mouse_set_absolute(p->x,p->y,65535,p->buttons,p->wheel);
    else mouse_inject_relative(p->x,p->y,p->buttons,p->wheel);
}
static void release_keys(struct input *i) {
    for(unsigned u=0;u<256;u++) if(i->state.keys[u/8]&(1U<<(u%8))) keyboard_inject_hid_usage(u,0);
    memset(&i->state,0,sizeof i->state);i->anchored=0;
    mouse_inject_relative(0,0,0,0);
}
static void worker(void) {
    for(;;) {
        for(unsigned n=0;n<input_count;n++) {
            struct input *i=&inputs[n];if(!i->online) continue;
            int rc=i2c_hid_read(&i->transport,&i->descriptor,i->packet,sizeof i->packet);
            if(rc>0) {
                struct hid_input_sink sink={i,key,pointer};
                (void)hid_report_decode(&i->layout,&i->state,i->packet+2,rc,&sink);
            } else if(rc==-1) {
                i->online=0;release_keys(i);
                kprintf("I2C-HID: %04x:%04x stopped after %s\n",i->descriptor.vendor,
                    i->descriptor.product,"bus timeout");
            }
            /* A zero-length idle read is ambiguous without GPIO IRQ state.
             * Keep held keys; only explicit initialization consumes RESET ACK. */
        }
        sched_lock();sched_block_current_locked(&input_wait);
    }
}
void i2c_hid_tick(void) {
    if(!__atomic_load_n(&running,__ATOMIC_ACQUIRE)) return;
    sched_lock();wait_queue_wake_one(&input_wait);sched_unlock();
}
static uacpi_iteration_decision discover(void *ctx,uacpi_namespace_node *node,uacpi_u32 depth) {
    (void)ctx;(void)depth;
    for(unsigned n=0;n<input_count;n++) if(inputs[n].node==node) return UACPI_ITERATION_DECISION_CONTINUE;
    if(input_count==MAX_INPUTS) return UACPI_ITERATION_DECISION_BREAK;
    struct connection c={.node=node};
    if(uacpi_for_each_device_resource(node,"_CRS",resource,&c)!=UACPI_STATUS_OK || c.count!=1
        || !c.bus || c.speed<100000 || c.speed>400000) {
        kprintf("I2C-HID: unsupported controller/resources\n");return UACPI_ITERATION_DECISION_CONTINUE;
    }
    uint16_t reg;
    if(!descriptor_register(node,&reg)) {kprintf("I2C-HID: descriptor _DSM unavailable\n");return UACPI_ITERATION_DECISION_CONTINUE;}
    uacpi_status s=uacpi_execute_simple(node,"_PS0");
    if(s!=UACPI_STATUS_OK && s!=UACPI_STATUS_NOT_FOUND) return UACPI_ITERATION_DECISION_CONTINUE;
    struct input *i=&inputs[input_count];
    i->bus=c.bus;i->address=c.address;i->speed=c.speed;i->node=node;
    i->transport=(struct i2c_hid_transport){i,xfer,clock_ms,sleep_ms};
    int rc=i2c_hid_start(&i->transport,reg,&i->descriptor,i->report_descriptor,sizeof i->report_descriptor);
    if(rc || hid_report_parse(&i->layout,i->report_descriptor,i->descriptor.report_length)) {
        kprintf("I2C-HID: initialization/report descriptor failed (%d)\n",rc);i->node=NULL;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    int supported=0;
    for(unsigned n=0;n<i->layout.field_count;n++) {
        struct hid_field *f=&i->layout.fields[n];
        if(f->page==7 || (f->application>=2 && f->application<=4 && f->page==1
            && (f->usage==0x30 || f->usage==0x31))) supported=1;
    }
    if(!supported) {
        kprintf("I2C-HID: report has no supported keyboard/pointer fields\n");
        i->node=NULL;return UACPI_ITERATION_DECISION_CONTINUE;
    }
    i->online=1;input_count++;
    kprintf("I2C-HID: %04x:%04x at 0x%x, %u report fields, polling\n",i->descriptor.vendor,
        i->descriptor.product,i->address,i->layout.field_count);
    return UACPI_ITERATION_DECISION_CONTINUE;
}
void i2c_hid_init(void) {
    dw_i2c_init();
    uacpi_find_devices("PNP0C50",discover,NULL);
    uacpi_find_devices("ACPI0C50",discover,NULL);
    if(input_count) {
        if(process_create_kthread(worker,NULL)) __atomic_store_n(&running,1,__ATOMIC_RELEASE);
        else {
            for(unsigned n=0;n<input_count;n++) inputs[n].online=0;
            kprintf("I2C-HID: input worker allocation failed\n");
        }
    }
}
