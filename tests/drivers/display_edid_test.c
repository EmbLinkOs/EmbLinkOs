#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "display/edid.h"
#include "../../user/lib/embk_monitor.h"
static void checksum(unsigned char *e) {
    unsigned sum=0;for(unsigned i=0;i<127;i++)sum+=e[i];e[127]=(unsigned char)-sum;
}
int main(void) {
    unsigned char e[128]={0,255,255,255,255,255,255,0};
    /* Independently encoded DEL monitor and standard 148.5 MHz 1080p timing. */
    e[8]=0x10;e[9]=0xac;e[18]=1;e[19]=4;e[21]=52;e[22]=29;e[24]=2;
    const unsigned char timing[18]={0x02,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40,
        0x58,0x2c,0x45,0,0xfd,0x1e,0x11,0,0,0x1e};
    memcpy(e+54,timing,18);e[75]=0xfc;memcpy(e+77,"Studio 24    ",13);
    e[93]=0xff;memcpy(e+95,"A12345\n      ",13);checksum(e);
    struct display_identity d;
    assert(!display_edid_parse(e,sizeof e,&d));
    assert(!strcmp(d.manufacturer,"DEL") && !strcmp(d.name,"Studio 24") && !strcmp(d.serial,"A12345"));
    assert(d.preferred_width==1920 && d.preferred_height==1080 && d.refresh_millihz==60000);
    assert(d.width_mm==509 && d.height_mm==286);
    struct embk_monitor_info info={.mode_count=2,.modes={{1280,800,0,0},{1920,1080,0,0}}};
    display_identity_describe(&d,&info);
    assert(info.flags&EMBK_MONITOR_EDID);assert(info.modes[1].flags&EMBK_MONITOR_PREFERRED);
    assert(info.modes[1].refresh_millihz==60000 && !info.modes[0].flags);
    for(unsigned n=0;n<128;n++)assert(display_edid_parse(e,n,&d)<0);
    assert(display_edid_parse(NULL,128,&d)<0 && display_edid_parse(e,128,NULL)<0);
    for(unsigned i=0;i<128;i++){e[i]^=1;assert(display_edid_parse(e,128,&d)<0);e[i]^=1;}
    e[8]=0;e[9]=0;checksum(e);assert(display_edid_parse(e,128,&d)<0);
    e[8]=0x10;e[9]=0xac;e[54+17]|=0x80;checksum(e);
    assert(!display_edid_parse(e,128,&d) && !d.preferred_width); /* Interlaced timing. */
    e[54+17]&=0x7f;e[77]=0x01;e[78]=0xff;checksum(e);
    assert(!display_edid_parse(e,128,&d) && d.name[0]=='?' && d.name[1]=='?');
    e[18]=2;checksum(e);assert(display_edid_parse(e,128,&d)<0);
    unsigned seed=0x3c5d71u;
    for(unsigned run=0;run<10000;run++) {
        for(unsigned i=10;i<127;i++){seed=seed*1664525u+1013904223u;e[i]=(unsigned char)(seed>>24);}
        e[18]=1;e[19]=4;checksum(e);
        assert(!display_edid_parse(e,128,&d));
        assert(d.name[63]==0 && d.serial[23]==0 && d.manufacturer[3]==0);
        assert(d.preferred_width<=8192 && d.preferred_height<=4320);
        assert(!d.preferred_width || (d.refresh_millihz>=20000 && d.refresh_millihz<=360000));
    }
    puts("EDID: real timing, identity, checksum corruption in every byte, truncation, text bounds, interlace refusal and 10k malformed descriptors OK");
}
