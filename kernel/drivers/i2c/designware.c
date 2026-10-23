/* Intel LPSS DesignWare I2C (120-MHz SPT/CNL/TGL-family controllers).
 * PCI IDs are explicit: unrelated SMBus controllers must never be programmed
 * as DesignWare. Firmware chooses the slave address; no blind bus scans. */
#include "drivers/i2c/designware.h"
#include "drivers/bus/pci.h"
#include "drivers/timer/timer.h"
#include "process/ksync.h"
#include "include/kprintf.h"
#include "mm/vmm.h"
#define MAX_BUSES 12
struct dw_i2c_bus {
    struct dw_i2c_io io;
    struct mutex lock;
    volatile uint8_t *regs;
    uint8_t bus, device, function;
    int used, failed;
};
static struct dw_i2c_bus buses[MAX_BUSES];
static uint32_t read_reg(void *p, uint32_t r) { return *(volatile uint32_t *)(((struct dw_i2c_bus *)p)->regs+r); }
static void write_reg(void *p, uint32_t r, uint32_t v) { *(volatile uint32_t *)(((struct dw_i2c_bus *)p)->regs+r) = v; }
static uint64_t now(void *p) { (void)p; return time_get_us(); }
static void pause_dw(void *p) { (void)p; __asm__ volatile("pause"); }
static int supported(uint16_t id) {
    return (id >= 0x9d60 && id <= 0x9d65) || (id >= 0xa160 && id <= 0xa165)
        || (id >= 0x9de8 && id <= 0x9deb) || (id >= 0x02e8 && id <= 0x02eb)
        || (id >= 0x34e8 && id <= 0x34eb) || (id >= 0x43e8 && id <= 0x43eb)
        || (id >= 0x51e8 && id <= 0x51eb) || (id >= 0x7ae8 && id <= 0x7aeb);
}
static void power_d0(const struct pci_device *p) {
    if (!(pci_read16(p->bus,p->device,p->function,PCI_STATUS)&0x10)) return;
    uint8_t at = pci_read8(p->bus,p->device,p->function,PCI_CAP_PTR)&0xfc;
    for (unsigned i=0; at>=0x40 && at<=0xf8 && i<48; i++) {
        if (pci_read8(p->bus,p->device,p->function,at)==1) {
            uint16_t pm = pci_read16(p->bus,p->device,p->function,at+4);
            /* PME status is W1C: preserve it by writing zero to bit 15. */
            pci_write16(p->bus,p->device,p->function,at+4,pm & ~0x8003U);
            uint64_t start=time_get_us();
            while (time_get_us()-start<10000) __asm__ volatile("pause");
            return;
        }
        at=pci_read8(p->bus,p->device,p->function,at+1)&0xfc;
    }
}
void dw_i2c_init(void) {
    unsigned count=0;
    for (unsigned i=0; i<pci_devices_count() && count<MAX_BUSES; i++) {
        const struct pci_device *p=pci_get_device(i);
        if (p->vendor_id!=0x8086 || !supported(p->device_id)) continue;
        struct pci_bar bar=pci_read_bar(p->bus,p->device,p->function,0);
        if (!bar.valid || !bar.is_mmio || bar.size<0x1000) continue;
        power_d0(p);
        pci_write16(p->bus,p->device,p->function,PCI_COMMAND,
            pci_read16(p->bus,p->device,p->function,PCI_COMMAND)|2);
        struct dw_i2c_bus *b=&buses[count];
        b->regs=(volatile uint8_t *)vmm_map_mmio(bar.address,bar.size);
        if (!b->regs) continue;
        /* LPSS private reset: release APB and functional reset, preserve DMA. */
        write_reg(b,0x204,read_reg(b,0x204)|3);
        if (read_reg(b,0xfc)!=0x44570140) {
            vmm_unmap_mmio((uint64_t)b->regs,bar.size); b->regs=NULL; continue;
        }
        write_reg(b,0x30,0); write_reg(b,0x88,0); /* IRQ and DMA disabled */
        write_reg(b,0x6c,0);
        uint64_t start=time_get_us();
        while ((read_reg(b,0x9c)&1) && time_get_us()-start<100000) __asm__ volatile("pause");
        if (read_reg(b,0x9c)&1) {
            vmm_unmap_mmio((uint64_t)b->regs,bar.size); b->regs=NULL; continue;
        }
        unsigned depth=((read_reg(b,0xf4)>>8)&255)+1;
        b->io=(struct dw_i2c_io){b,read_reg,write_reg,now,pause_dw,depth,0};
        b->bus=p->bus; b->device=p->device; b->function=p->function;
        mutex_init(&b->lock); b->used=1; count++;
        kprintf("I2C: Intel %04x at %u:%u.%u, RX FIFO %u\n",p->device_id,p->bus,p->device,p->function,depth);
    }
}
struct dw_i2c_bus *dw_i2c_find(unsigned bus,unsigned dev,unsigned fn) {
    for (unsigned i=0;i<MAX_BUSES;i++) if (buses[i].used && buses[i].bus==bus
        && buses[i].device==dev && buses[i].function==fn) return &buses[i];
    return NULL;
}
int dw_i2c_xfer(struct dw_i2c_bus *b,uint16_t addr,uint32_t speed,
    const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn) {
    if (!b || !b->used || speed<100000 || speed>400000) return -3;
    mutex_lock(&b->lock);
    if (b->failed) { mutex_unlock(&b->lock); return -1; }
    /* Conservative 120MHz counts. Fast timing is 333kHz, standard ~90kHz;
     * both stay below the firmware's advertised speed. */
    unsigned fast=speed>=400000;
    write_reg(b,0x00,1|(fast?4:2)|(1<<5)|(1<<6));
    write_reg(b,0x14,600); write_reg(b,0x18,720);
    write_reg(b,0x1c,120); write_reg(b,0x20,240);
    write_reg(b,0x7c,36); /* 300ns SDA hold */
    write_reg(b,0x38,0); write_reg(b,0x3c,0);
    int rc=dw_i2c_transfer(&b->io,addr,w,wn,r,rn);
    if (rc==-1) { b->failed=1; kprintf("I2C: controller timeout; bus stopped\n"); }
    mutex_unlock(&b->lock);
    return rc;
}
