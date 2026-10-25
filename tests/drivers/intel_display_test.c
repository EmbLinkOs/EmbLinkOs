#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/video/intel_display.c"
static uint32_t regs[REGS_SIZE/4];
static struct boot_protocol bp={.firmware=BOOT_FW_UEFI,.fb_addr=0xe0001000,.fb_width=1920,.fb_height=1080,.fb_pitch=7680,.fb_bpp=32,.fb_format=1};
static struct pci_device devices[2]={{.vendor_id=0x8086,.device_id=0x5916,.class_code=3},{.vendor_id=0x8086,.device_id=0x9d48,.class_code=6,.subclass=1}};
static unsigned mappings,unmappings;
const struct boot_protocol *boot_protocol_get(void){return &bp;}
uint32_t pci_devices_count(void){return 2;}
const struct pci_device *pci_get_device(uint32_t i){return &devices[i];}
uint16_t pci_read16(uint8_t b,uint8_t d,uint8_t f,uint8_t o){(void)b;(void)d;(void)f;(void)o;return 2;}
struct pci_bar pci_read_bar(uint8_t b,uint8_t d,uint8_t f,uint8_t i){(void)b;(void)d;(void)f;return (struct pci_bar){.address=i==2?0xe0000000:0xf0000000,.size=i==2?0x10000000:REGS_SIZE,.is_mmio=1,.valid=1};}
uint64_t vmm_map_mmio(uint64_t p,uint64_t n){assert(p==0xf0000000&&n==REGS_SIZE);mappings++;return (uintptr_t)regs;}
void vmm_unmap_mmio(uint64_t p,uint64_t n){assert(p==(uintptr_t)regs&&n==REGS_SIZE);unmappings++;}
void spin_lock(spinlock_t *l){assert(!l->locked);l->locked=1;}
void spin_unlock(spinlock_t *l){assert(l->locked);l->locked=0;}
void kprintf(const char *f,...){(void)f;}
static void fixture(void){memset(regs,0,sizeof regs);regs[0x70180/4]=(1u<<31)|(4u<<24);regs[0x70188/4]=120;regs[0x70190/4]=(1079u<<16)|1919;regs[0x7019c/4]=0x1000;regs[PWM_CTL/4]=(1u<<31)|(1u<<29);regs[PWM_FREQ/4]=(1000u<<16)|600;}
int main(void){
    fixture();const struct gpu_driver *d=intel_display_probe();assert(d && mappings==1);
    struct gpu_mode m;assert(d->get_mode(&m)&&m.width==1920&&m.phys_addr==bp.fb_addr&&!d->set_mode);
    unsigned p;assert(!intel_backlight_get(&p)&&p==60);assert(!intel_backlight_set(37));
    assert(regs[PWM_FREQ/4]==((1000u<<16)|370)&&regs[PWM_CTL/4]==((1u<<31)|(1u<<29)));
    assert(intel_backlight_set(101)==-EMBK_EINVAL);
    regs[PWM_CTL/4]=0;assert(intel_backlight_set(40)==-EMBK_ENODEV);
    fixture();regs[0x70180/4]|=1u<<10;assert(!intel_display_probe()&&unmappings);
    fixture();bp.fb_addr++;assert(!intel_display_probe());bp.fb_addr--;
    fixture();devices[0].device_id=0x1234;unsigned before=mappings;assert(!intel_display_probe()&&mappings==before);
    fixture();devices[0].device_id=0x3ea0;devices[1].device_id=0xa30d;
    regs[PWM_FREQ/4]=100000;regs[PWM_DUTY/4]=70000;assert(intel_display_probe());
    assert(!intel_backlight_get(&p)&&p==70);assert(!intel_backlight_set(25));
    assert(regs[PWM_FREQ/4]==100000&&regs[PWM_DUTY/4]==25000);
    puts("Intel display: linear firmware plane/aperture validation, SPT/CNP native PWM, preserved frequency/polarity, unsupported modes refused OK");
}
