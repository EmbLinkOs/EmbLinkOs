#include "edid.h"
#include "include/kstring.h"
#include "../../user/lib/embk_monitor.h"
static void text(char *out,size_t cap,const uint8_t *p,unsigned n) {
    size_t at=0;for(unsigned i=0;i<n && at+1<cap;i++) {
        if(p[i]==10 || p[i]==0)break;
        out[at++]=p[i]>=32 && p[i]<127?(char)p[i]:'?';
    }
    while(at && out[at-1]==' ')at--;
    out[at]=0;
}
int display_edid_parse(const uint8_t *p,size_t n,struct display_identity *out) {
    static const uint8_t magic[8]={0,255,255,255,255,255,255,0};
    if(!p || !out || n<128 || memcmp(p,magic,8) || p[18]!=1 || p[19]>4)return -1;
    unsigned sum=0;for(unsigned i=0;i<128;i++)sum+=p[i];if(sum&255)return -1;
    struct display_identity d={0};uint16_t vendor=(uint16_t)p[8]<<8|p[9];
    for(unsigned i=0;i<3;i++) {unsigned c=(vendor>>(10-i*5))&31;if(!c || c>26)return -1;d.manufacturer[i]='A'+c-1;}
    d.width_mm=(uint32_t)p[21]*10;d.height_mm=(uint32_t)p[22]*10;
    for(unsigned i=0;i<4;i++) {
        const uint8_t *b=p+54+i*18;uint32_t clock=((uint32_t)b[1]<<8|b[0])*10000;
        if(!clock) {
            if(b[2] || b[4])continue;
            if(b[3]==0xfc)text(d.name,sizeof d.name,b+5,13);
            if(b[3]==0xff)text(d.serial,sizeof d.serial,b+5,13);
            continue;
        }
        if(d.preferred_width)continue;
        uint32_t w=b[2]|(uint32_t)(b[4]&0xf0)<<4,h=b[5]|(uint32_t)(b[7]&0xf0)<<4;
        uint32_t total_w=w+b[3]+((uint32_t)(b[4]&15)<<8),total_h=h+b[6]+((uint32_t)(b[7]&15)<<8);
        if(w<320 || h<200 || w>8192 || h>4320 || total_w<=w || total_h<=h || (b[17]&0x80))continue;
        uint64_t refresh=(uint64_t)clock*1000/(total_w*total_h);
        if(refresh<20000 || refresh>360000)continue;
        d.preferred_width=w;d.preferred_height=h;d.refresh_millihz=(uint32_t)refresh;
        uint32_t mmw=b[12]|(uint32_t)(b[14]&0xf0)<<4,mmh=b[13]|(uint32_t)(b[14]&15)<<8;
        if(mmw && mmh){d.width_mm=mmw;d.height_mm=mmh;}
    }
    *out=d;return 0;
}

void display_identity_describe(const struct display_identity *d,struct embk_monitor_info *info) {
    if(!d->manufacturer[0])return;
    info->flags|=EMBK_MONITOR_EDID;
    if(d->name[0])memcpy(info->name,d->name,sizeof info->name);
    memcpy(info->manufacturer,d->manufacturer,4);
    memcpy(info->serial,d->serial,sizeof info->serial);
    info->width_mm=d->width_mm;info->height_mm=d->height_mm;
    for(unsigned i=0;i<info->mode_count;i++) {
        struct embk_monitor_mode *m=&info->modes[i];
        if(m->width==d->preferred_width && m->height==d->preferred_height) {
            m->flags|=EMBK_MONITOR_PREFERRED;m->refresh_millihz=d->refresh_millihz;
        }
    }
}
