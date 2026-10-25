#include "hid_report.h"
static void zero(void *p, unsigned n) { uint8_t *b=p; while(n--) *b++=0; }
static int32_t sign_value(uint32_t v, unsigned bits) {
    if (bits && bits<32 && (v & (1U<<(bits-1)))) v |= ~((1U<<bits)-1);
    return (int32_t)v;
}
struct global {
    uint32_t page, size, count, id;
    int32_t min, max;
};
static int report_index(struct hid_report_layout *l, unsigned id) {
    for (unsigned i=0;i<l->report_count;i++) if(l->ids[i]==id) return i;
    if(l->report_count==HID_REPORTS) return -1;
    unsigned i=l->report_count++; l->ids[i]=id; return i;
}
int hid_report_parse(struct hid_report_layout *l,const uint8_t *d,unsigned n) {
    if (!l || !d || !n || n>4096) return -1;
    zero(l,sizeof(*l));
    struct global g={0}, stack[4]; unsigned sp=0;
    uint32_t usages[64], umin=0, umax=0; unsigned un=0, have_range=0;
    uint8_t apps[16]={0}; unsigned depth=0;
    for(unsigned at=0;at<n;) {
        unsigned prefix=d[at++], len=prefix&3, type=(prefix>>2)&3, tag=prefix>>4;
        if(prefix==0xfe) return -1; /* long items unsupported */
        if(len==3) len=4;
        if(len>n-at) return -1;
        uint32_t v=0; for(unsigned i=0;i<len;i++) v|=(uint32_t)d[at++]<<(8*i);
        if(type==1) {
            switch(tag) {
            case 0: if(v>0xffff) return -1; g.page=v; break;
            case 1: g.min=sign_value(v,len*8); break;
            case 2: g.max=g.min<0?sign_value(v,len*8):(int32_t)v; break;
            case 7: g.size=v; break;
            case 8: if(!v || v>255) return -1; g.id=v; l->numbered=1; break;
            case 9: g.count=v; break;
            case 10: if(sp==4) return -1; stack[sp++]=g; break;
            case 11: if(!sp) return -1; g=stack[--sp]; break;
            default: break;
            }
        } else if(type==2) {
            uint32_t usage=len==4?v:((g.page<<16)|(v&0xffff));
            if(tag==0) { if(un==64) return -1; usages[un++]=usage; }
            if(tag==1) { umin=usage; have_range|=1; }
            if(tag==2) { umax=usage; have_range|=2; }
        } else if(type==0) {
            if(tag==10) { /* Collection */
                if(depth==16) return -1;
                uint8_t app=depth?apps[depth-1]:0;
                uint32_t usage=un?usages[0]:umin;
                if(v==1) {
                    if(usage==0x10006) app=1; /* keyboard */
                    else if(usage==0x10002) app=2; /* mouse */
                    else if(usage==0xd0005) app=3; /* touchpad */
                    else if(usage==0xd0004) app=4; /* touchscreen */
                }
                apps[depth++]=app;
            } else if(tag==12) { if(!depth) return -1; depth--; }
            else if(tag==8) { /* Input; output/feature have separate bit offsets */
                if(!g.size || g.size>32 || !g.count || g.count>8192/g.size) return -1;
                int ri=report_index(l,g.id); if(ri<0) return -1;
                unsigned start=l->bits[ri], bits=g.size*g.count;
                if(start+bits>8192) return -1;
                l->bits[ri]+=bits;
                if(!(v&1)) {
                    unsigned fields=(v&2)?g.count:1;
                    if(fields>HID_FIELDS-l->field_count || g.min>g.max) return -1;
                    if(have_range && (have_range!=3 || umax<umin || (umin>>16)!=(umax>>16))) return -1;
                    for(unsigned i=0;i<fields;i++) {
                        uint32_t usage=un?usages[i<un?i:un-1]:umin;
                        if(have_range==3 && (v&2)) usage=umin+(i<umax-umin?i:umax-umin);
                        struct hid_field *f=&l->fields[l->field_count++];
                        f->bit=start+i*g.size; f->size=g.size; f->count=(v&2)?1:g.count;
                        f->page=usage>>16; f->usage=usage; f->usage_max=have_range==3?(umax&0xffff):f->usage;
                        f->report=ri; f->flags=v; f->minimum=g.min; f->maximum=g.max;
                        f->application=depth?apps[depth-1]:0;
                    }
                }
            }
            un=0; have_range=0; umin=umax=0;
        }
    }
    if(depth || sp || !l->field_count || !l->report_count) return -1;
    if(l->numbered) for(unsigned i=0;i<l->report_count;i++) if(!l->ids[i]) return -1;
    return 0;
}
static uint32_t extract(const uint8_t *p,unsigned bit,unsigned size) {
    uint32_t v=0;
    for(unsigned i=0;i<size;i++) v|=((p[(bit+i)/8]>>((bit+i)%8))&1U)<<i;
    return v;
}
int hid_report_decode(const struct hid_report_layout *l,struct hid_input_state *s,
    const uint8_t *data,unsigned n,const struct hid_input_sink *sink) {
    if(!l || !s || !data || !sink || !n) return -1;
    unsigned ri=0;
    if(l->numbered) {
        unsigned id=*data++; n--;
        for(ri=0;ri<l->report_count;ri++) if(l->ids[ri]==id) break;
        if(ri==l->report_count) return 0;
    }
    if(n<(l->bits[ri]+7U)/8) return -1;
    uint8_t keys[32]={0}; int keyboard=0,rollover=0;
    struct hid_pointer p={0}; int has_x=0,has_y=0,pointer=0,tip_seen=0,button_seen=0;
    for(unsigned i=0;i<l->field_count;i++) {
        const struct hid_field *f=&l->fields[i]; if(f->report!=ri) continue;
        if(f->page==7) {
            keyboard=1;
            for(unsigned j=0;j<f->count;j++) {
                uint32_t raw=extract(data,f->bit+j*f->size,f->size);
                int32_t value=f->minimum<0?sign_value(raw,f->size):(int32_t)raw;
                uint32_t usage=f->usage;
                if(!(f->flags&2)) {
                    if(value<f->minimum || value>f->maximum) continue;
                    int64_t mapped=(int64_t)f->usage+value-f->minimum;
                    if(mapped<0 || mapped>f->usage_max) continue;
                    usage=mapped;
                } else if(!value) continue;
                if(usage>=1 && usage<=3) rollover=1;
                else if(usage && usage<256) keys[usage/8]|=1U<<(usage%8);
            }
        }
        if(f->application<2 || f->application>4) continue;
        uint32_t raw=extract(data,f->bit,f->size);
        int32_t value=f->minimum<0?sign_value(raw,f->size):(int32_t)raw;
        if(f->page==9 && f->usage>=1 && f->usage<=3) {
            if(value) p.buttons|=1U<<(f->usage-1);
            pointer=1;button_seen=1;
        }
        if(f->page==0xd && f->usage==0x42 && !tip_seen) {p.touching=!!value;tip_seen=1;pointer=1;}
        if(f->page==1 && (f->usage==0x30 || f->usage==0x31 || f->usage==0x38)) {
            if(f->usage==0x38) {p.wheel=value;pointer=1;continue;}
            int *seen=f->usage==0x30?&has_x:&has_y;
            if(*seen) continue; /* primary contact; secondary contacts ignored */
            *seen=1;
            int32_t position=value;
            if(!(f->flags&4)) {
                int64_t range=(int64_t)f->maximum-f->minimum;
                if(range<=0) return -1;
                int64_t offset=(int64_t)value-f->minimum;
                if(offset<0) offset=0;
                if(offset>range) offset=range;
                position=offset*65535/range; p.absolute=1;
            }
            if(f->usage==0x30) p.x=position; else p.y=position;
            p.touchpad=f->application==3;pointer=1;
        }
    }
    if(keyboard && !rollover) {
        for(unsigned i=0;i<32;i++) s->report_keys[ri][i]=keys[i];
        zero(keys,32);
        for(unsigned j=0;j<l->report_count;j++) for(unsigned i=0;i<32;i++) keys[i]|=s->report_keys[j][i];
        /* Modifiers before other keys: a Shift+A report produces uppercase A. */
        for(unsigned pass=0;pass<2;pass++) for(unsigned u=pass?0:0xe0;u<(pass?0xe0:0xe8);u++) {
            unsigned mask=1U<<(u%8);
            if(((s->keys[u/8]^keys[u/8])&mask) && sink->key)
                sink->key(sink->ctx,u,!!(keys[u/8]&mask));
        }
        for(unsigned i=0;i<32;i++) s->keys[i]=keys[i];
    }
    if(pointer && sink->pointer) {
        if(!tip_seen) p.touching=1;
        else if(p.absolute && !p.touchpad) {
            if(p.touching) p.buttons|=1;
            button_seen=1;
        }
        if(button_seen) s->report_buttons[ri]=p.buttons;
        p.buttons=0;
        for(unsigned j=0;j<l->report_count;j++) p.buttons|=s->report_buttons[j];
        if(!p.absolute || (has_x && has_y)) sink->pointer(sink->ctx,&p);
    }
    return 0;
}
