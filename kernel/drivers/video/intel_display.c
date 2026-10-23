/* Gen9 display handoff and native PCH PWM. Preserve the firmware's trained
 * link, timings and GTT. This is not a render engine or a general modesetter.
 * Register definitions checked against Intel's Gen9 display layout and Linux
 * 6.12 intel_backlight_regs.h / skl_universal_plane_regs.h. */
#include "drivers/video/intel_display.h"
#include "drivers/video/framebuffer.h"
#include "drivers/bus/pci.h"
#include "boot/boot_protocol.h"
#include "include/spinlock.h"
#include "mm/vmm.h"
#include "include/kprintf.h"
#include "include/errno.h"

#define REGS_SIZE 0x100000u
#define PWM_CTL 0xc8250u
#define PWM_FREQ 0xc8254u
#define PWM_DUTY 0xc8258u
static volatile uint8_t *registers;
static struct gpu_mode current;
static unsigned pwm_kind;
static spinlock_t pwm_lock=SPINLOCK_INIT;
#ifndef INTEL_MMIO_READ
#define INTEL_MMIO_READ(base,offset) (*(volatile uint32_t *)((base)+(offset)))
#define INTEL_MMIO_WRITE(base,offset,value) (*(volatile uint32_t *)((base)+(offset))=(value))
#endif
static uint32_t rd(unsigned off) {return INTEL_MMIO_READ(registers,off);}
static void wr(unsigned off,uint32_t value) {INTEL_MMIO_WRITE(registers,off,value);}
static int supported(uint16_t id) {
    switch(id) {
    case 0x1902:case 0x1906:case 0x190b:case 0x1912:case 0x1916:case 0x191b:
    case 0x191d:case 0x1926:case 0x1927:case 0x193b:
    case 0x5902:case 0x5906:case 0x590b:case 0x5912:case 0x5916:case 0x5917:
    case 0x591b:case 0x591d:case 0x5926:case 0x5927:
    case 0x3e90:case 0x3e91:case 0x3e92:case 0x3e9b:case 0x3ea0:case 0x3ea5:
    case 0x3ea6:case 0x3ea8:case 0x9b41:case 0x9bc4:case 0x9bc5:case 0x9bc8:return 1;
    default:return 0;
    }
}
static unsigned pch_layout(void) {
    for(unsigned i=0;i<pci_devices_count();i++) {
        const struct pci_device *d=pci_get_device(i);
        if(!d || d->vendor_id!=0x8086 || d->class_code!=6 || d->subclass!=1)continue;
        unsigned family=d->device_id&0xff80;
        if(family==0x9d00 || family==0xa100 || family==0xa180 || family==0xa280)return 1;
        if(family==0xa300 || family==0x9d80 || family==0x0280 || family==0x0680)return 2;
    }
    return 0;
}
static int pwm_read(unsigned *percent,uint32_t *frequency,uint32_t *duty) {
    if(!registers || !pwm_kind || !(rd(PWM_CTL)&(1u<<31)))return -EMBK_ENODEV;
    uint32_t freq=rd(PWM_FREQ),level;
    if(pwm_kind==1) {level=freq&0xffff;freq>>=16;}else level=rd(PWM_DUTY);
    if(!freq || level>freq)return -EMBK_EIO;
    if(frequency)*frequency=freq;
    if(duty)*duty=level;
    if(percent)*percent=(unsigned)(((uint64_t)level*100+freq/2)/freq);
    return 0;
}
int intel_backlight_get(unsigned *percent) {
    spin_lock(&pwm_lock);int rc=pwm_read(percent,NULL,NULL);spin_unlock(&pwm_lock);return rc;
}
int intel_backlight_set(unsigned percent) {
    if(percent>100)return -EMBK_EINVAL;
    spin_lock(&pwm_lock);uint32_t freq=0;
    int rc=pwm_read(NULL,&freq,NULL);
    if(!rc) {
        uint32_t level=(uint32_t)(((uint64_t)freq*percent+50)/100);
        if(percent && !level)level=1;
        /* Change only duty. Never change frequency, enable, polarity, panel
         * power sequencing or the firmware's selected PWM controller. */
        if(pwm_kind==1) wr(PWM_FREQ,(freq<<16)|level);else wr(PWM_DUTY,level);
        uint32_t read_freq,read_level;
        rc=pwm_read(NULL,&read_freq,&read_level);
        if(!rc && (read_freq!=freq || read_level!=level))rc=-EMBK_EIO;
    }
    spin_unlock(&pwm_lock);return rc;
}
static bool get_mode(struct gpu_mode *out) {*out=current;return true;}
static const struct gpu_driver driver={.name="Intel Gen9 display (firmware mode)",.get_mode=get_mode};
const struct gpu_driver *intel_display_probe(void) {
    const struct boot_protocol *bp=boot_protocol_get();
    registers=NULL;pwm_kind=0;
    if(!bp || bp->firmware!=BOOT_FW_UEFI || bp->fb_bpp!=32 || !bp->fb_addr ||
       !bp->fb_width || !bp->fb_height || bp->fb_width>8192 || bp->fb_height>4320 ||
       bp->fb_pitch<(uint64_t)bp->fb_width*4 || bp->fb_pitch>65536 || bp->fb_format>1)return NULL;
    for(unsigned i=0;i<pci_devices_count();i++) {
        const struct pci_device *d=pci_get_device(i);
        if(!d || d->vendor_id!=0x8086 || d->class_code!=3 || !supported(d->device_id))continue;
        /* Do not touch an Intel adapter that is powered down or whose memory
         * decoder the firmware did not enable. */
        if(!(pci_read16(d->bus,d->device,d->function,PCI_COMMAND)&2))continue;
        struct pci_bar mmio=pci_read_bar(d->bus,d->device,d->function,0);
        struct pci_bar aperture=pci_read_bar(d->bus,d->device,d->function,2);
        uint64_t size=(uint64_t)bp->fb_pitch*bp->fb_height;
        if(!mmio.valid || !mmio.is_mmio || mmio.size<REGS_SIZE ||
           !aperture.valid || !aperture.is_mmio || bp->fb_addr<aperture.address ||
           bp->fb_addr-aperture.address>=aperture.size || size>aperture.size-(bp->fb_addr-aperture.address))continue;
        registers=(volatile uint8_t *)vmm_map_mmio(mmio.address,REGS_SIZE);
        if(!registers)continue;
        unsigned matches=0;
        for(unsigned pipe=0;pipe<3;pipe++) {
            unsigned base=0x70180+pipe*0x1000;
            uint32_t ctl=rd(base),plane_size=rd(base+0x10);
            if(!(ctl&(1u<<31)) || ((ctl>>24)&15)!=4 || (ctl&((7u<<10)|(1u<<15)|(1u<<8)|3u)))continue;
            if((ctl&(1u<<20)?FB_FORMAT_RGB:FB_FORMAT_BGR)!=bp->fb_format || rd(base+0x24))continue;
            if((plane_size&0xffff)+1!=bp->fb_width || (plane_size>>16)+1!=bp->fb_height ||
               (rd(base+8)&0xfff)*64!=bp->fb_pitch)continue;
            if((rd(base+0x1c)&0xfffff000u)!=bp->fb_addr-aperture.address)continue;
            matches++;
        }
        if(!matches) {vmm_unmap_mmio((uint64_t)registers,REGS_SIZE);registers=NULL;continue;}
        current=(struct gpu_mode){bp->fb_addr,bp->fb_width,bp->fb_height,bp->fb_pitch,32,bp->fb_format};
        pwm_kind=pch_layout();unsigned percent;
        if(intel_backlight_get(&percent))pwm_kind=0;
        kprintf("intel-display: %04x firmware scanout verified, %ux%u, %u pipe(s), PWM %s\n",
            d->device_id,current.width,current.height,matches,pwm_kind?"available":"unavailable");
        return &driver;
    }
    return NULL;
}
