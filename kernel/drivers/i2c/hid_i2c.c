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
#include "acpi/aml.h"
#include "acpi/serial_i2c.h"
#define MAX_INPUTS 8
struct input {
    struct dw_i2c_bus *bus;
    struct i2c_hid_transport transport;
    struct i2c_hid_descriptor descriptor;
    struct hid_report_layout layout;
    struct hid_input_state state;
    struct aml_node *node;
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
static struct aml_node *local_method(struct aml_node *n,const char *path) {
    struct aml_node *m=aml_lookup(n,path);return m && m->parent==n?m:NULL;
}
static int integer(struct aml_node *n,const char *path,uint64_t *out) {
    return local_method(n,path) && aml_eval_integer(n,path,out);
}
static struct dw_i2c_bus *pci_controller(struct aml_node *node) {
    struct aml_node *path[16];unsigned depth=0,bus=0;
    for(struct aml_node *n=node;n;n=n->parent) {
        if(depth==16) return NULL;
        path[depth++]=n;
    }
    while(depth) {
        struct aml_node *n=path[--depth];uint64_t value;
        if(integer(n,"_SEG",&value) && value) return NULL;
        if(integer(n,"_BBN",&value)) {
            if(value>255) return NULL;
            bus=value;
        }
        struct aml_object *hid=aml_evaluate(local_method(n,"_HID"),NULL,0);
        int root=hid && ((hid->kind==AML_INTEGER && (hid->u.integer==0x030ad041 || hid->u.integer==0x080ad041)) ||
            (hid->kind==AML_STRING && (!strcmp((const char *)hid->u.buf.data,"PNP0A03") || !strcmp((const char *)hid->u.buf.data,"PNP0A08"))));
        aml_put(hid);
        if(root) continue; /* Root _ADR, when present, is not a PCI bridge. */
        if(!integer(n,"_ADR",&value)) continue;
        if(value>0x1f0007 || (value&0xffff)>7 || (value>>16)>31) return NULL;
        unsigned device=value>>16,function=value&7;
        if(n==node) return dw_i2c_find(bus,device,function);
        if(pci_read8(bus,device,function,0x0b)!=6 || pci_read8(bus,device,function,0x0a)!=4) return NULL;
        bus=pci_read8(bus,device,function,0x19);
    }
    return NULL;
}
static int descriptor_register(struct aml_node *node,uint16_t *out) {
    const uint8_t guid[]={0xf7,0xf6,0xdf,0x3c,0x67,0x42,0x55,0x45,0xad,0x05,0xb3,0x0a,0x3d,0x89,0x38,0xde};
    struct aml_object *args[4]={aml_buffer(guid,sizeof guid),aml_integer(1),aml_integer(1),aml_package(0)};
    int ok=args[0] && args[1] && args[2] && args[3];
    struct aml_object *value=ok?aml_evaluate(local_method(node,"_DSM"),args,4):NULL;
    ok=value && value->kind==AML_INTEGER && value->u.integer<=0xffff;
    if(ok)*out=value->u.integer;
    aml_put(value);for(unsigned i=0;i<4;i++)aml_put(args[i]);return ok;
}
static int xfer(void *ctx,const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn) {
    struct input *i=ctx;return dw_i2c_xfer(i->bus,i->address,i->speed,w,wn,r,rn);
}
static uint64_t clock_ms(void *ctx) {(void)ctx;return time_get_us()/1000;}
static void sleep_ms(void *ctx,unsigned ms) {(void)ctx;sched_sleep_ms(ms);}
static void key(void *ctx,uint16_t usage,int down) {(void)ctx;keyboard_inject_hid_usage(usage,down);}
static void pointer(void *ctx,const struct hid_pointer *p) {
    struct input *i=ctx;
    if(p->absolute && p->touchpad) {
        int32_t dx=0,dy=0;
        if(p->touching && i->anchored) {dx=(p->x-i->x)/64;dy=(p->y-i->y)/64;}
        i->anchored=p->touching;i->x=p->x;i->y=p->y;
        mouse_move_relative(dx,dy,p->buttons,p->wheel);
    } else if(p->absolute) mouse_set_absolute(p->x,p->y,65535,p->buttons,p->wheel);
    else mouse_move_relative(p->x,p->y,p->buttons,p->wheel);
}
static void release_keys(struct input *i) {
    for(unsigned u=0;u<256;u++) if(i->state.keys[u/8]&(1U<<(u%8))) keyboard_inject_hid_usage(u,0);
    memset(&i->state,0,sizeof i->state);i->anchored=0;
    mouse_move_relative(0,0,0,0);
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
static bool discover(struct aml_node *node,void *ctx) {
    (void)ctx;
    uint64_t sta;
    if(local_method(node,"_STA") && (!integer(node,"_STA",&sta) || (sta&0xb)!=0xb))return true;
    for(unsigned n=0;n<input_count;n++) if(inputs[n].node==node) return true;
    if(input_count==MAX_INPUTS) return false;
    struct aml_object *resources=aml_evaluate(local_method(node,"_CRS"),NULL,0);
    struct acpi_i2c_connection c={0};
    int supported=resources && resources->kind==AML_BUFFER &&
        !acpi_i2c_connection(resources->u.buf.data,resources->u.buf.len,&c);
    struct dw_i2c_bus *bus=supported?pci_controller(aml_lookup(node,c.source)):NULL;
    aml_put(resources);
    if(!bus){kprintf("I2C-HID: unsupported controller/resources\n");return true;}
    uint16_t reg;
    if(!descriptor_register(node,&reg)) {kprintf("I2C-HID: descriptor _DSM unavailable\n");return true;}
    struct aml_node *ps0=local_method(node,"_PS0");
    if(ps0)aml_put(aml_evaluate(ps0,NULL,0));
    struct input *i=&inputs[input_count];
    i->bus=bus;i->address=c.address;i->speed=c.speed;i->node=node;
    i->transport=(struct i2c_hid_transport){i,xfer,clock_ms,sleep_ms};
    int rc=i2c_hid_start(&i->transport,reg,&i->descriptor,i->report_descriptor,sizeof i->report_descriptor);
    if(rc || hid_report_parse(&i->layout,i->report_descriptor,i->descriptor.report_length)) {
        kprintf("I2C-HID: initialization/report descriptor failed (%d)\n",rc);i->node=NULL;
        return true;
    }
    supported=0;
    for(unsigned n=0;n<i->layout.field_count;n++) {
        struct hid_field *f=&i->layout.fields[n];
        if(f->page==7 || (f->application>=2 && f->application<=4 && f->page==1
            && (f->usage==0x30 || f->usage==0x31))) supported=1;
    }
    if(!supported) {
        kprintf("I2C-HID: report has no supported keyboard/pointer fields\n");
        i->node=NULL;return true;
    }
    i->online=1;input_count++;
    kprintf("I2C-HID: %04x:%04x at 0x%x, %u report fields, polling\n",i->descriptor.vendor,
        i->descriptor.product,i->address,i->layout.field_count);
    return true;
}
void i2c_hid_init(void) {
    dw_i2c_init();
    aml_for_each_hid("PNP0C50",discover,NULL);
    aml_for_each_hid("ACPI0C50",discover,NULL);
    if(input_count) {
        if(process_create_kthread(worker,NULL)) __atomic_store_n(&running,1,__ATOMIC_RELEASE);
        else {
            for(unsigned n=0;n<input_count;n++) inputs[n].online=0;
            kprintf("I2C-HID: input worker allocation failed\n");
        }
    }
}
