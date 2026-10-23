/* Intel High Definition Audio 1.0a, PCI class 04/03. One analog stereo route,
 * CORB/RIRB verbs and a 32-page output BDL at 48 kHz/S16. HDMI, DSP firmware,
 * input and codec-specific laptop amplifier quirks need separate support.
 * MSI has a dedicated vector; no legacy IRQ handler is overwritten. */
#include "drivers/audio/hda.h"
#include "drivers/audio/pcm.h"
#include "drivers/bus/pci.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "drivers/timer/timer.h"
#include "include/spinlock.h"
#include "arch/x86_64/irq/idt.h"
#include "arch/x86_64/irq/lapic.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#define HDA_VECTOR 0x53
#define HDA_RING 32
#define HDA_PAGE 4096
struct hda_bd {uint64_t address;uint32_t length,flags;};
struct widget {uint32_t caps;uint8_t links[32],count;};
struct hda_device {
    uint32_t corb[256] __attribute__((aligned(4096)));
    volatile uint64_t rirb[256] __attribute__((aligned(4096)));
    struct hda_bd bdl[HDA_RING] __attribute__((aligned(4096)));
    int16_t pcm[HDA_RING][2048] __attribute__((aligned(4096)));
    struct widget widgets[256];
    uint8_t path[16],choices[16],visited[256],valid[HDA_RING];
    volatile uint8_t *regs;
    unsigned corb_size,rirb_size,wp,rp,sd,stream,retired,base,codec,fg,depth,pin,dac;
    uint32_t fg_pcm,fg_format,fg_inamp,fg_outamp;
    uint64_t physical;
    struct pci_device pci;
    uint64_t underruns;
    int present,running,fault,exhausted;
};
static struct hda_device *h;
static spinlock_t lock=SPINLOCK_INIT;
extern void hda_irq_stub(void);
#ifndef HDA_MMIO_READ
#define HDA_MMIO_READ(base,o,width) (*(volatile uint##width##_t *)((base)+(o)))
#define HDA_MMIO_WRITE(base,o,width,v) (*(volatile uint##width##_t *)((base)+(o))=(v))
#endif
static uint8_t r8(unsigned o) {return HDA_MMIO_READ(h->regs,o,8);}
static uint16_t r16(unsigned o) {return HDA_MMIO_READ(h->regs,o,16);}
static uint32_t r32(unsigned o) {return HDA_MMIO_READ(h->regs,o,32);}
static void w8(unsigned o,uint8_t v) {HDA_MMIO_WRITE(h->regs,o,8,v);}
static void w16(unsigned o,uint16_t v) {HDA_MMIO_WRITE(h->regs,o,16,v);}
static void w32(unsigned o,uint32_t v) {HDA_MMIO_WRITE(h->regs,o,32,v);}
static void delay(unsigned us) {uint64_t start=time_get_us();while(time_get_us()-start<us) __asm__ volatile("pause");}
static int wait32(unsigned o,uint32_t mask,uint32_t value) {
    uint64_t start=time_get_us();
    while((r32(o)&mask)!=value) {if(time_get_us()-start>20000) return -1;__asm__ volatile("pause");}
    return 0;
}
static int wait16(unsigned o,uint16_t mask,uint16_t value) {
    uint64_t start=time_get_us();
    while((r16(o)&mask)!=value) {if(time_get_us()-start>20000) return -1;__asm__ volatile("pause");}
    return 0;
}
static uint64_t phys(const void *p) {return h->physical+(uintptr_t)p-(uintptr_t)h;}
static void address(unsigned o,const void *p) {uint64_t a=phys(p);w32(o,a);w32(o+4,a>>32);}
static int command(unsigned nid,unsigned verb,unsigned data,uint32_t *out) {
    if(h->fault || nid>255 || data>65535) return -1;
    unsigned next=(h->wp+1)%h->corb_size;
    uint64_t start=time_get_us();
    while((r16(0x4a)&255)==next) if(time_get_us()-start>20000) goto fail;
    h->corb[next]=(h->codec<<28)|(nid<<20)|(verb<<8)|data;
    __atomic_thread_fence(__ATOMIC_RELEASE);h->wp=next;w16(0x48,next);
    start=time_get_us();
    for(;;) {
        if(r8(0x5d)&4) goto fail; /* RIRB overflow loses response ownership. */
        unsigned wp=r16(0x58)&255;
        while(h->rp!=wp) {
            h->rp=(h->rp+1)%h->rirb_size;__atomic_thread_fence(__ATOMIC_ACQUIRE);
            uint64_t reply=h->rirb[h->rp];unsigned ext=reply>>32;
            if(ext&16) continue; /* unsolicited responses are not command replies */
            if((ext&15)!=h->codec) goto fail;
            if(out) *out=(uint32_t)reply;
            /* Acknowledge each solicited completion even while controller
             * interrupts are masked. QEMU also uses this to re-arm CORB. */
            w8(0x5d,1);
            return 0;
        }
        if(time_get_us()-start>20000) goto fail;
        __asm__ volatile("pause");
    }
fail:kprintf("hda: verb timeout nid %u verb %x CORBRP %x RIRBWP %x status %x\n",nid,verb,r16(0x4a),r16(0x58),r8(0x5d));h->fault=1;return -1;
}
static int parameter(unsigned nid,unsigned id,uint32_t *v) {return command(nid,0xf00,id,v);}
static int power(unsigned nid) {
    if(command(nid,0x705,0,NULL)) return -1;
    uint64_t start=time_get_us();uint32_t v;
    do {if(command(nid,0xf05,0,&v)) return -1;if(!(v&0xff)) return 0;delay(100);} while(time_get_us()-start<20000);
    return -1;
}
static int links(unsigned nid) {
    struct widget *w=&h->widgets[nid];uint32_t length;
    if(!(w->caps&(1u<<8))) return 0;
    if(parameter(nid,0x0e,&length)) return -1;
    unsigned n=length&127,wide=length&128,previous=0;int have=0;
    for(unsigned i=0;i<n;) {
        uint32_t packed;if(command(nid,0xf02,i,&packed)) return -1;
        unsigned per=wide?2:4;
        for(unsigned k=0;k<per && i<n;k++,i++) {
            unsigned raw=(packed>>(k*(wide?16:8)))&(wide?65535:255);
            unsigned range=raw&(wide?32768:128),node=raw&(wide?32767:127);
            if(!node || node>255 || (range && (!have || node<=previous))) return -1;
            unsigned first=range?previous+1:node;
            for(unsigned j=first;j<=node;j++) {if(w->count==32) return -1;w->links[w->count++]=j;}
            previous=node;have=1;
        }
    }
    return 0;
}
static int route(unsigned nid,unsigned depth) {
    if(!nid || depth==16 || h->visited[nid]) return 0;
    struct widget *w=&h->widgets[nid];unsigned type=(w->caps>>20)&15;
    if(w->caps&(1u<<9)) return 0; /* digital converters/pins are not analog */
    h->visited[nid]=1;h->path[depth]=nid;
    if(type==0 && (w->caps&1)) {
        uint32_t pcm=h->fg_pcm,format=h->fg_format;
        if((w->caps&(1u<<4)) && (parameter(nid,0x0a,&pcm)||parameter(nid,0x0b,&format))) return 0;
        if((pcm&(1u<<6)) && (pcm&(1u<<17)) && (format&1)) {h->dac=nid;h->depth=depth+1;return 1;}
    }
    if(type==2 || type==3 || type==4) for(unsigned i=0;i<w->count;i++) {
        if(route(w->links[i],depth+1)) {h->choices[depth]=i;return 1;}
    }
    return 0;
}
static int amplifier(unsigned nid,int input,unsigned index,int mute) {
    struct widget *w=&h->widgets[nid];uint32_t cap=input?h->fg_inamp:h->fg_outamp;
    if(!(w->caps&(input?2u:4u))) return 0;
    if((w->caps&8) && parameter(nid,input?0x0d:0x12,&cap)) return -1;
    unsigned gain=cap&127,steps=(cap>>8)&127;if(gain>steps) gain=steps;
    if(index>15) return -1;
    return command(nid,0x300,(input?0x4000:0x8000)|0x3000|(index<<8)|(mute?128:gain),NULL);
}
static int codec_route(unsigned codec) {
    h->codec=codec;uint32_t roots;
    if(parameter(0,4,&roots)) return -1;
    unsigned start=(roots>>16)&255,count=roots&255;
    kprintf("hda: codec %u function groups %u + %u\n",codec,start,count);
    if(!start || start+count>256) return -1;
    for(unsigned f=start;f<start+count;f++) {
        uint32_t type,nodes;
        if(parameter(f,5,&type)) return -1;
        if((type&255)!=1) continue;
        h->fg=f;
        if(power(f) || parameter(f,4,&nodes) || parameter(f,0x0a,&h->fg_pcm) ||
           parameter(f,0x0b,&h->fg_format) || parameter(f,0x0d,&h->fg_inamp) || parameter(f,0x12,&h->fg_outamp)) return -1;
        unsigned first=(nodes>>16)&255,n=nodes&255;
        kprintf("hda: group %u nodes %u + %u PCM %x formats %x\n",f,first,n,h->fg_pcm,h->fg_format);
        if(!first || first+n>256) return -1;
        memset(h->widgets,0,sizeof h->widgets);
        for(unsigned i=first;i<first+n;i++) {
            if(parameter(i,9,&h->widgets[i].caps) || links(i)) return -1;
        }
        /* Prefer the built-in speaker, then headphones, then line out. */
        static const unsigned devices[]={1,2,0};
        for(unsigned d=0;d<3;d++) for(unsigned i=first;i<first+n;i++) {
            if(((h->widgets[i].caps>>20)&15)!=4) continue;
            uint32_t cfg,pincap;
            if(command(i,0xf1c,0,&cfg) || parameter(i,0x0c,&pincap)) return -1;
            if((cfg>>30)==1 || ((cfg>>20)&15)!=devices[d] || !(pincap&16)) continue;
            memset(h->visited,0,sizeof h->visited);
            if(!route(i,0)) continue;
            h->pin=i;
            for(unsigned j=0;j<h->depth;j++) {
                unsigned node=h->path[j];struct widget *w=&h->widgets[node];
                if((w->caps&(1u<<10)) && power(node)) return -1;
                if(w->count && command(node,0x701,h->choices[j],NULL)) return -1;
                if(amplifier(node,0,0,0)) return -1;
                if(w->caps&2) for(unsigned k=0;k<(w->count?w->count:1);k++)
                    if(amplifier(node,1,k,w->count && k!=h->choices[j])) return -1;
            }
            if(command(i,0x707,0x40|((pincap&8)?0x80:0),NULL)) return -1;
            if((pincap&(1u<<16)) && command(i,0x70c,2,NULL)) return -1;
            if(command(h->dac,0x200,0x11,NULL) || command(h->dac,0x706,0x10,NULL)) return -1;
            return 0;
        }
    }
    return -1;
}
static int ring_size(unsigned reg,unsigned *size) {
    uint8_t v=r8(reg);unsigned sel;
    if(v&0x40) {*size=256;sel=2;} else if(v&0x20) {*size=16;sel=1;} else if(v&0x10) {*size=2;sel=0;} else return -1;
    w8(reg,sel);return 0;
}
static int program(unsigned first);
static unsigned current_buffer(void) {return h->running?(h->base+r32(h->sd+4)/HDA_PAGE)%HDA_RING:h->retired;}
static void stop_dma(void) {w8(h->sd,r8(h->sd)&~2u);h->running=0;}
static void retire(void) {
    if(!h || !h->present || !h->running) return;
    unsigned status=r8(h->sd+3);unsigned current=current_buffer();
    w8(h->sd+3,status&0x1c);
    if(status&0x18) {h->fault=1;stop_dma();return;}
    while(h->retired!=current) {
        h->valid[h->retired]=0;memset(h->pcm[h->retired],0,HDA_PAGE);
        h->retired=(h->retired+1)%HDA_RING;
    }
    if(!h->valid[current]) {h->exhausted=1;stop_dma();}
}
void hda_irq_handler(void) {
    spin_lock(&lock);retire();spin_unlock(&lock);lapic_send_eoi();
}
void hda_init(void) {
    for(unsigned i=0;i<pci_devices_count();i++) {
        const struct pci_device *p=pci_get_device(i);
        if(!p || p->class_code!=4 || p->subclass!=3) continue;
        struct pci_bar bar=pci_read_bar(p->bus,p->device,p->function,0);
        if(!bar.valid || !bar.is_mmio || bar.size<0x400) continue;
        if(!h) {
            unsigned pages=(sizeof(*h)+4095)/4096;uint64_t base=pmm_alloc_contiguous(pages,1);
            if(!base) return;
            h=(struct hda_device *)P2V(base);memset(h,0,pages*4096);h->physical=base;
        }
        uint64_t base=h->physical;memset(h,0,sizeof *h);h->physical=base;h->pci=*p;
        h->regs=(volatile uint8_t *)vmm_map_mmio(bar.address,bar.size);if(!h->regs) continue;
        uint16_t pc=pci_read16(p->bus,p->device,p->function,PCI_COMMAND);
        pci_write16(p->bus,p->device,p->function,PCI_COMMAND,pc|6|0x400);
        unsigned stage=1;
        unsigned cap=r16(0),inputs=(cap>>8)&15,outputs=(cap>>12)&15;
        if(!outputs || (!(cap&1) && h->physical+sizeof *h>UINT32_MAX)) goto fail;
        h->stream=inputs;h->sd=0x80+inputs*0x20;
        if(h->sd+0x20>bar.size) goto fail;
        w32(0x20,0);w8(0x4c,0);w8(0x5c,0);
        w32(8,r32(8)&~1u);if(wait32(8,1,0)) goto fail;delay(100);
        w32(8,r32(8)|1u);if(wait32(8,1,1)) goto fail;delay(1000);
        stage=2;uint16_t codecs=r16(0x0e);w16(0x0c,0);
        if(!codecs || ring_size(0x4e,&h->corb_size) || ring_size(0x5e,&h->rirb_size)) goto fail;
        address(0x40,h->corb);address(0x50,(const void *)h->rirb);
        w16(0x48,0);w16(0x4a,0x8000);
        if(wait16(0x4a,0x8000,0x8000)) goto fail;
        w16(0x4a,0);if(wait16(0x4a,0x8000,0)) goto fail;
        w16(0x58,0x8000);h->rp=r16(0x58)&255;w16(0x5a,1);w8(0x5d,5);
        w8(0x5c,3);w8(0x4c,2);
        stage=3;int found=0;
        for(unsigned c=0;c<15;c++) if(codecs&(1u<<c)) {if(!codec_route(c)) {found=1;break;}if(h->fault) break;}
        if(!found) goto fail;
        stage=4;idt_set_entry(HDA_VECTOR,(uint64_t)hda_irq_stub,0x8e);
        if(!pci_enable_msi(p->bus,p->device,p->function,HDA_VECTOR,(uint8_t)lapic_get_id())) goto fail;
        for(unsigned b=0;b<HDA_RING;b++) h->bdl[b]=(struct hda_bd){phys(h->pcm[b]),HDA_PAGE,1};
        h->present=1;hda_stop();
        w32(0x20,0x80000000u|(1u<<h->stream));
        kprintf("hda: analog output codec %u pin %u DAC %u, 48000 Hz stereo, MSI\n",h->codec,h->pin,h->dac);return;
fail:
        w32(0x20,0);w8(0x4c,0);w8(0x5c,0);if(h->sd) w8(h->sd,0);
        pci_write16(p->bus,p->device,p->function,PCI_COMMAND,(pc|0x400)&~4u);
        kprintf("hda: controller %x:%x.%x setup failed at stage %u (GCAP %x, CORBRP %x)\n",p->bus,p->device,p->function,stage,cap,r16(0x4a));
    }
}
bool hda_present(void) {return h && h->present && !h->fault;}
const char *hda_output_name(void) {return "High Definition Audio";}
uint32_t hda_fill(int index,const int16_t *frames,uint32_t n) {
    if(!hda_present() || index<0 || index>=HDA_RING || !frames) return 0;
    spin_lock(&lock);retire();
    if(h->fault || h->valid[index] || (h->running && current_buffer()==(unsigned)index)) {spin_unlock(&lock);return 0;}
    if(n>1024) n=1024;
    memcpy(h->pcm[index],frames,n*4);memset((uint8_t *)h->pcm[index]+n*4,0,HDA_PAGE-n*4);
    __atomic_thread_fence(__ATOMIC_RELEASE);h->valid[index]=1;spin_unlock(&lock);return n;
}
uint8_t hda_civ(void) {
    if(!hda_present()) return 0;
    spin_lock(&lock);retire();unsigned index=current_buffer();spin_unlock(&lock);return index;
}
static void start(void) {
    if(h->fault || h->running || !h->valid[h->retired]) return;
    if(h->exhausted){h->underruns++;h->exhausted=0;}
    if(program(h->retired)) {h->fault=1;return;}
    __atomic_thread_fence(__ATOMIC_RELEASE);w8(h->sd,0x1e);h->running=1;
}
void hda_play(int last) {(void)last;if(!hda_present()) return;spin_lock(&lock);h->exhausted=0;h->underruns=0;start();spin_unlock(&lock);}
void hda_set_last(int last) {(void)last;if(!hda_present()) return;spin_lock(&lock);retire();if(!h->running) start();spin_unlock(&lock);}
bool hda_done(int last) {
    (void)last;if(!hda_present()) return true;
    spin_lock(&lock);retire();int any=0;for(unsigned i=0;i<HDA_RING;i++) any|=h->valid[i];spin_unlock(&lock);return !any;
}
/* Restart at the next logical page using a rotated BDL. Both real HDA and
 * QEMU start from offset zero after this explicit stream reset; an underrun
 * therefore cannot replay old pages or depend on a controller's RUN semantics. */
static int program(unsigned first) {
    if(wait32(h->sd,2,0)) return -1;
    w8(h->sd,1);if(wait32(h->sd,1,1)) return -1;
    w8(h->sd,0);if(wait32(h->sd,1,0)) return -1;
    h->base=first;
    for(unsigned i=0;i<HDA_RING;i++) h->bdl[i]=(struct hda_bd){phys(h->pcm[(first+i)%HDA_RING]),HDA_PAGE,1};
    address(h->sd+0x18,h->bdl);w32(h->sd+8,HDA_RING*HDA_PAGE);w16(h->sd+0x0c,HDA_RING-1);
    w16(h->sd+0x12,0x11);w8(h->sd+2,0x10);w8(h->sd+3,0x1c);return 0;
}
void hda_stop(void) {
    if(!h || !h->present) return;
    spin_lock(&lock);stop_dma();
    if(wait32(h->sd,2,0)) h->fault=1;
    h->exhausted=0;h->retired=0;memset(h->valid,0,sizeof h->valid);memset(h->pcm,0,sizeof h->pcm);
    spin_unlock(&lock);
}

/* Retain bounded CORB/RIRB transactions and interrupt-backed page retirement
 * behind the upstream PCM interface. HDA always plays the padded whole page. */
static bool hda_pcm_init(void) {hda_init();return hda_present();}
static uint32_t hda_pcm_rate(void) {return hda_present()?48000:0;}
static uint32_t hda_pcm_frames(void) {return 1024;}
static uint32_t hda_pcm_desc_frames(int i) {(void)i;return 1024;}
static uint64_t hda_pcm_underruns(void) {return h?h->underruns:0;}
const struct pcm_driver hda_driver={.name="hda",.init=hda_pcm_init,
    .sample_rate=hda_pcm_rate,.frames_per_buffer=hda_pcm_frames,
    .fill=hda_fill,.desc_frames=hda_pcm_desc_frames,.civ=hda_civ,
    .set_last=hda_set_last,.play=hda_play,.done=hda_done,.stop=hda_stop,
    .underruns=hda_pcm_underruns};
