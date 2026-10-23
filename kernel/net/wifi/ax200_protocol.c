#include "ax200_protocol.h"
#include "include/kstring.h"
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
int ax200_fw_parse(struct ax200_fw *fw,const uint8_t *p,unsigned n) {
    if(!fw || !p) return -1;
    memset(fw,0,sizeof(*fw));
    if(n<88 || n>AX200_FW_MAX || rd32(p) || rd32(p+4)!=0x0a4c5749 || rd32(p+72)!=77) return -1;
    unsigned at=88,group=0,phy_seen=0,cmd_seen=0;uint8_t api_seen=0,capa_seen=0;
    while(at<n) {
        if(n-at<8) goto bad;
        uint32_t type=rd32(p+at),len=rd32(p+at+4);at+=8;
        if(len>n-at || len>UINT32_MAX-3 || ((len+3)&~3U)>n-at) goto bad;
        const uint8_t *data=p+at;at+=(len+3)&~3U;
        if(type==19) { /* SEC_RT, in self-loading context DRAM order */
            if(len<4) goto bad;
            uint32_t offset=rd32(data);
            if(offset==0xffffcccc || offset==0xaaaabbbb) {
                if(len>8 || (offset==0xffffcccc?group!=0:group!=1) || !fw->count[group]) goto bad;
                group++;continue;
            }
            if(len==4 || len>32768+4 || fw->count[group]==AX200_FW_SECTIONS) goto bad;
            struct ax200_fw_section *s=&fw->sections[group][fw->count[group]++];
            s->data=data+4;s->length=len-4;
        } else if(type==23) { /* PHY_SKU */
            if(len!=4 || phy_seen++) goto bad;
            fw->phy=rd32(data);
        } else if(type==29 || type==30) { /* API changes / capabilities */
            if(len!=8) goto bad;
            unsigned index=rd32(data);uint32_t flags=rd32(data+4);
            if(type==29) {
                if(index>=4 || (api_seen&(1U<<index))) goto bad;
                api_seen|=1U<<index;fw->api[index]=flags;
            } else {
                if(index>=8 || (capa_seen&(1U<<index))) goto bad;
                capa_seen|=1U<<index;fw->capabilities[index]=flags;
            }
        } else if(type==48) { /* CMD_VERSIONS */
            if(cmd_seen++ || (len&3) || len/4>AX200_FW_COMMANDS) goto bad;
            fw->command_count=len/4;memcpy(fw->commands,data,len);
        }
    }
    if(group!=2 || !fw->count[0] || !fw->count[1] || !fw->count[2] || !phy_seen || !cmd_seen) goto bad;
    return 0;
bad:
    memset(fw,0,sizeof(*fw));return -1;
}
unsigned ax200_fw_command_version(const struct ax200_fw *f,unsigned group,unsigned cmd,int notification) {
    if(!f || group>255 || cmd>255) return 255;
    for(unsigned i=0;i<f->command_count;i++) if(f->commands[i][0]==cmd && f->commands[i][1]==group)
        return f->commands[i][notification?3:2];
    return 255;
}
int ax200_rx_records(const uint8_t *p,unsigned n,int (*record)(void *,const uint8_t *,unsigned),void *arg) {
    if(!p || !record || n>4096) return -1;
    unsigned at=0;
    while(n-at>=8) {
        uint32_t flags=rd32(p+at),len=flags&0x3fff;
        if(flags==0x55550000 || (!flags && !rd32(p+at+4))) return 0;
        if(len<4 || len>n-at-4 || record(arg,p+at+4,len)) return -1;
        unsigned advance=(len+4+63)&~63U;
        if(advance>n-at) return len+4==n-at?0:-1;
        at+=advance;
    }
    return 0;
}
