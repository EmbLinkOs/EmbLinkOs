/* Run the actual HDA transport against register/codec/DMA fixtures. QEMU WAV
 * acceptance is separate: this model checks failure and lifetime paths too. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static uint32_t model_read(volatile uint8_t *,unsigned,unsigned);
static void model_write(volatile uint8_t *,unsigned,unsigned,uint32_t);
#define HDA_MMIO_READ(base,o,width) model_read(base,o,width)
#define HDA_MMIO_WRITE(base,o,width,v) model_write(base,o,width,v)
#include "drivers/audio/hda.c"
static struct hda_device memory;
static uint8_t registers[0x400];
static uint64_t clock_us;
static unsigned commands,eoi_count,msi_ok=1,missing_reply,malformed,command_flags;
static int response_pending;
static void put(unsigned at,unsigned width,uint32_t value) {memcpy(registers+at,&value,width/8);}
static uint32_t get(unsigned at,unsigned width) {uint32_t value=0;memcpy(&value,registers+at,width/8);return value;}
void kprintf(const char *f,...) {(void)f;}
void spin_lock(spinlock_t *l) {assert(!l->locked);l->locked=1;}
void spin_unlock(spinlock_t *l) {assert(l->locked);l->locked=0;}
void spinlock_init(spinlock_t *l) {l->locked=0;}
uint64_t time_get_us(void) {clock_us+=100;return clock_us;}
void hda_irq_stub(void) {}
void idt_set_entry(uint8_t vector,uint64_t handler,uint8_t flags) {assert(vector==0x53 && handler && flags==0x8e);}
void lapic_send_eoi(void) {eoi_count++;}
uint32_t lapic_get_id(void) {return 0;}
uint64_t pmm_alloc_contiguous(uint64_t pages,uint64_t alignment) {
    assert(pages==(sizeof memory+4095)/4096 && alignment==1);return V2P(&memory);
}
uint64_t vmm_map_mmio(uint64_t address,uint64_t length) {assert(address==0x100000 && length==sizeof registers);return (uintptr_t)registers;}
uint32_t pci_devices_count(void) {return 1;}
const struct pci_device *pci_get_device(uint32_t i) {assert(!i);static const struct pci_device p={.class_code=4,.subclass=3};return &p;}
struct pci_bar pci_read_bar(uint8_t b,uint8_t d,uint8_t f,uint8_t index) {
    (void)b;(void)d;(void)f;assert(!index);return (struct pci_bar){.address=0x100000,.size=sizeof registers,.valid=true,.is_mmio=true};
}
uint16_t pci_read16(uint8_t b,uint8_t d,uint8_t f,uint8_t off) {(void)b;(void)d;(void)f;assert(off==4);return command_flags;}
void pci_write16(uint8_t b,uint8_t d,uint8_t f,uint8_t off,uint16_t v) {(void)b;(void)d;(void)f;assert(off==4);command_flags=v;}
bool pci_enable_msi(uint8_t b,uint8_t d,uint8_t f,uint8_t vector,uint8_t cpu) {
    (void)b;(void)d;(void)f;assert(vector==0x53 && !cpu);return msi_ok;
}
static uint32_t reply(unsigned nid,unsigned verb,unsigned data) {
    if(verb==0xf00) {
        if(data==4) return nid==0?0x00010001:0x00020002;
        if(data==5) return 1;
        if(data==0x0a) return (1u<<6)|(1u<<17);
        if(data==0x0b) return 1;
        if(data==9) return nid==3?(4u<<20)|(1u<<8):1;
        if(data==0x0e) return malformed?33:1;
        if(data==0x0c) return (1u<<4)|(1u<<16);
        return 0;
    }
    if(verb==0xf02) return 2;
    if(verb==0xf1c) return 1u<<20;
    return 0;
}
static uint32_t model_read(volatile uint8_t *base,unsigned o,unsigned width) {
    assert(base==registers && o+width/8<=sizeof registers);return get(o,width);
}
static void model_write(volatile uint8_t *base,unsigned o,unsigned width,uint32_t value) {
    assert(base==registers && o+width/8<=sizeof registers);
    if(o==0x5d) {if(value&1) response_pending=0;put(o,width,get(o,width)&~value);return;}
    if(o==0x58 && (value&0x8000)) {put(o,width,0);return;}
    if(o==0x4e || o==0x5e) {put(o,width,0x40|(value&3));return;}
    if(o==0x48 && h && (get(0x4c,8)&2)) {
        assert(!response_pending);assert((command_flags&6)==6);
        unsigned cmd=h->corb[value],nid=(cmd>>20)&255,verb=(cmd>>8)&0xfff,data=cmd&255;
        put(0x4a,16,value);commands++;
        if(missing_reply) return;
        unsigned wp=(get(0x58,16)+1)%h->rirb_size;
        h->rirb[wp]=reply(nid,verb,data);put(0x58,16,wp);put(0x5d,8,1);response_pending=1;return;
    }
    if(h && o==h->sd+3) {put(o,width,get(o,width)&~value);return;}
    if(h && o==h->sd && (value&2) && !(get(o,8)&2)) put(o+4,32,0);
    put(o,width,value);
}
static void reset(void) {
    memset(&memory,0,sizeof memory);memset(registers,0,sizeof registers);h=NULL;
    command_flags=commands=response_pending=0;clock_us=0;
    put(0,16,0x1101);put(0x0e,16,1);put(0x4e,8,0x40);put(0x5e,8,0x40);
}
static void advance(void) {
    assert(h->running);unsigned position=(get(h->sd+4,32)+4096)%(32*4096);
    put(h->sd+4,32,position);put(h->sd+3,8,4);hda_irq_handler();
}
int main(void) {
    reset();hda_init();assert(hda_present() && h->pin==3 && h->dac==2);
    for(unsigned i=0;i<1100;i++) {uint32_t out;assert(!parameter(2,0x0a,&out) && out==0x20040);}
    assert(commands>1100 && !response_pending);
    int16_t frames[2048];for(unsigned i=0;i<2048;i++) frames[i]=i;
    for(unsigned i=0;i<8;i++) assert(hda_fill(i,frames,1024)==1024);
    hda_play(7);assert(h->running);
    for(unsigned i=0;i<1000;i++) {
        advance();assert(hda_fill((i+8)%32,frames,1024)==1024);
    }
    for(unsigned i=0;i<8;i++) advance();
    assert(hda_done(0) && !h->running && eoi_count==1008);
    unsigned next=hda_civ();assert(hda_fill(next,frames,1024)==1024);hda_set_last(next);
    assert(h->running && h->base==next && h->bdl[0].address==V2P(h->pcm[next]));advance();assert(hda_done(next));
    hda_stop();assert(hda_civ()==0);assert(hda_fill(0,frames,1024)==1024);hda_play(0);advance();assert(hda_done(0));
    reset();missing_reply=1;hda_init();assert(!hda_present() && !(command_flags&4));missing_reply=0;
    reset();malformed=1;hda_init();assert(!hda_present() && !(command_flags&4));malformed=0;
    reset();msi_ok=0;hda_init();assert(!hda_present() && !(command_flags&4));
    puts("HDA transport: codec route, 1100 verbs, 1000 DMA pages, underrun/reopen and fail-stop OK");
}
