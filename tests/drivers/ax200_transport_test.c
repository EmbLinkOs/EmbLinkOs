/* Firmware/MMIO model, not a physical-card claim. Run the real transport and
 * station against API-77 command layouts, DMA notifications and a scripted AP.
 * The WPA2 packets are the independent Python fixtures, not driver-generated
 * expectations. This also exercises queue wrapping and fail-stop lifetime. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
static uint32_t model_read(volatile uint8_t *,unsigned);
static void model_write(volatile uint8_t *,unsigned,uint32_t);
#define AX200_MMIO_READ(base,reg) model_read(base,reg)
#define AX200_MMIO_WRITE(base,reg,value) model_write(base,reg,value)
#include "net/wifi/ax200.c"
#include "wifi_vectors.h"
static uint32_t registers[2048],pci_command;
static uint64_t clock_us;
static unsigned produced,commands,transmits,deliveries,port_up;
static int kicked,announced,drop_reply,bad_firmware,absent,radio_blocked;
static uint8_t blob[AX200_FW_MAX],last_frame[WIFI_MPDU_MAX],ether_frame[1514];
static unsigned blob_size,last_length,ether_length;
static struct wifi_station client;
static struct ax200_device model_device;
static void *dma(uint64_t address) {return (void *)(uintptr_t)(address+DIRECT_MAP_BASE);}
uint64_t pmm_alloc_contiguous(uint64_t n,uint64_t align) {
    assert(n==(sizeof model_device+4095)/4096 && align==1);return V2P(&model_device);
}
void kprintf(const char *fmt,...) {(void)fmt;}
uint64_t time_get_us(void) {clock_us+=1000;return clock_us;}
uint64_t tsc_get_freq_hz(void) {return UINT64_C(1000000000);}
uint64_t vmm_map_mmio(uint64_t address,uint64_t length) {assert(address==0x100000 && length==8192);return (uintptr_t)registers;}
uint32_t pci_devices_count(void) {return absent?0:1;}
const struct pci_device *pci_get_device(uint32_t index) {
    static const struct pci_device p={.vendor_id=0x8086,.device_id=0x2723,.class_code=2,.subclass=0x80};
    assert(index==0);return &p;
}
struct pci_bar pci_read_bar(uint8_t b,uint8_t d,uint8_t f,uint8_t i) {
    (void)b;(void)d;(void)f;assert(i==0);
    return (struct pci_bar){.address=0x100000,.size=8192,.is_mmio=true,.valid=true};
}
uint8_t pci_read8(uint8_t b,uint8_t d,uint8_t f,uint8_t reg) {(void)b;(void)d;(void)f;(void)reg;return 0;}
uint16_t pci_read16(uint8_t b,uint8_t d,uint8_t f,uint8_t reg) {(void)b;(void)d;(void)f;assert(reg==PCI_COMMAND);return pci_command;}
void pci_write16(uint8_t b,uint8_t d,uint8_t f,uint8_t reg,uint16_t val) {(void)b;(void)d;(void)f;assert(reg==PCI_COMMAND);pci_command=val;}
int vfs_stat(const char *path,struct vfs_stat *s) {assert(!strcmp(path,FW_PATH));memset(s,0,sizeof(*s));s->size=blob_size;return 0;}
int vfs_read(const char *path,uint64_t off,void *out,size_t n,size_t *got) {
    assert(!strcmp(path,FW_PATH) && !off && n==blob_size);memcpy(out,blob,n);*got=n;
    if(bad_firmware) ((uint8_t *)out)[0]^=1;
    return 0;
}
static void emit(unsigned id,unsigned idx,unsigned qid,const void *p,unsigned n) {
    assert(n<=4088 && kicked && announced);
    unsigned slot=produced&511;uint64_t entry=ax.rx_free[slot];assert((entry&4095)==slot);
    uint8_t *target=dma(entry&~UINT64_C(4095));assert(target==ax.rx[slot]);
    memset(target,0,4096);uint32_t size=n+4;memcpy(target,&size,4);
    target[4]=id;target[5]=id>>8;target[6]=idx;target[7]=qid;if(n) memcpy(target+8,p,n);
    ax.rx_used[slot]=slot;produced++;ax.rx_status.closed_rb_num=produced&4095;
    registers[IWX_CSR_INT/4]|=IWX_CSR_INT_BIT_FH_RX;
}
static void mpdu(const uint8_t *frame,unsigned n) {
    uint8_t packet[WIFI_MPDU_MAX+IWX_RX_DESC_SIZE_V1];memset(packet,0,IWX_RX_DESC_SIZE_V1);
    struct iwx_rx_mpdu_desc *d=(void *)packet;d->mpdu_len=n;
    d->status=IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK;
    d->v1.channel=1;d->v1.gp2_on_air_rise=100000;
    memcpy(packet+IWX_RX_DESC_SIZE_V1,frame,n);emit(IWX_REPLY_RX_MPDU_CMD,0,0x80,packet,n+IWX_RX_DESC_SIZE_V1);
}
static void management_header(uint8_t *p,unsigned subtype,const uint8_t *dst) {
    memset(p,0,24);p[0]=subtype;memcpy(p+4,dst,6);memcpy(p+10,vector_ap,6);memcpy(p+16,vector_ap,6);
}
static void ap_beacon(void) {
    uint8_t p[128],bcast[6]={255,255,255,255,255,255};management_header(p,0x80,bcast);
    memset(p+24,0,12);p[32]=100;p[34]=0x31;p[35]=4;unsigned at=36;
    memcpy(p+at,"\0\4IEEE\1\4\202\204\213\226\3\1\1",15);at+=15;
    memcpy(p+at,vector_rsn,sizeof vector_rsn);at+=sizeof vector_rsn;
    mpdu(p,at);
}
static void ap_eapol(const uint8_t *e,unsigned n) {
    uint8_t p[WIFI_MPDU_MAX];management_header(p,0x08,vector_sta);p[1]=2;
    memcpy(p+24,"\252\252\3\0\0\0\210\216",8);memcpy(p+32,e,n);mpdu(p,n+32);
}
static void ap_receive(const uint8_t *frame,unsigned n) {
    assert(n>=24 && n<=sizeof last_frame);memcpy(last_frame,frame,n);last_length=n;
    uint8_t p[128];
    if(frame[0]==0xb0) {
        assert(n==30 && frame[26]==1);management_header(p,0xb0,vector_sta);
        memset(p+24,0,6);p[26]=2;mpdu(p,30);
    } else if(frame[0]==0) {
        assert(!memcmp(frame+n-sizeof vector_rsn,vector_rsn,sizeof vector_rsn));
        management_header(p,0x10,vector_sta);p[24]=0x31;p[25]=4;p[26]=p[27]=0;p[28]=42;p[29]=0xc0;
        memcpy(p+30,"\1\4\202\204\213\226",6);mpdu(p,36);ap_eapol(vector_m1,sizeof vector_m1);
    } else if(!(frame[1]&0x40) && n>=32 && frame[30]==0x88 && frame[31]==0x8e) {
        if(n==32+sizeof vector_m2 && !memcmp(frame+32,vector_m2,sizeof vector_m2)) ap_eapol(vector_m3,sizeof vector_m3);
        else assert(n==32+sizeof vector_m4 && !memcmp(frame+32,vector_m4,sizeof vector_m4));
    }
}
static void firmware_command(unsigned pointer) {
    unsigned queue=pointer>>16,index=(pointer-1)&255;uint8_t wire[4096];unsigned n=0;
    const struct iwx_tfh_tfd *t=queue?&ax.tx_desc[index]:&ax.cmd_desc[index];
    assert(t->num_tbs>=1 && t->num_tbs<=3 && t->tbs[0].tb_len<=20);
    for(unsigned i=0;i<t->num_tbs;i++) {
        assert(t->tbs[i].tb_len<=4092 && n+t->tbs[i].tb_len<=sizeof wire);
        memcpy(wire+n,dma(t->tbs[i].addr),t->tbs[i].tb_len);n+=t->tbs[i].tb_len;
    }
    if(drop_reply) return;
    if(queue) {
        assert(queue==1 && wire[0]==IWX_TX_CMD && wire[2]==index && wire[3]==queue);
        const struct iwx_tx_cmd_gen2 *cmd=(void *)(wire+4);
        assert(cmd->flags&(IWX_TX_FLAGS_CMD_RATE|IWX_TX_FLAGS_ENCRYPT_DIS));
        assert(n==4+sizeof(*cmd)+cmd->len && ax.tx_bc.tfd_offset[index]==(cmd->len+3)/4);
        uint8_t reply[sizeof(struct iwx_tx_resp)+4]={0};struct iwx_tx_resp *r=(void *)reply;
        r->frame_count=1;r->status.status=IWX_TX_STATUS_SUCCESS;uint32_t ssn=pointer&255;
        memcpy(reply+sizeof(*r),&ssn,4);transmits++;emit(IWX_TX_CMD,index,queue,reply,sizeof reply);
        ap_receive(wire+4+sizeof(*cmd),cmd->len);return;
    }
    const struct iwx_cmd_header_wide *h=(const void *)wire;
    assert(h->qid==0 && h->idx==index && n==8U+h->length);commands++;
    unsigned id=(h->group_id<<8)|h->opcode;uint8_t result[1024]={0};unsigned size=4;
    switch(id) {
    case IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP,IWX_NVM_GET_INFO): {
        struct iwx_nvm_get_info_rsp *r=(void *)result;r->general.n_hw_addrs=1;r->mac_sku.mac_sku_flags=3;
        r->phy_sku.tx_chains=r->phy_sku.rx_chains=3;r->regulatory.lar_enabled=1;r->regulatory.n_channels=51;
        for(unsigned i=0;i<51;i++) r->regulatory.channel_profile[i]=IWX_NVM_CHANNEL_VALID;
        size=sizeof(*r);break;
    }
    case IWX_WIDE_ID(1,IWX_MCC_UPDATE_CMD): {
        struct iwx_mcc_update_resp_v4 *r=(void *)result;r->mcc=('U'<<8)|'S';r->n_channels=51;
        for(unsigned i=0;i<51;i++) r->channels[i]=IWX_NVM_CHANNEL_VALID|IWX_NVM_CHANNEL_ACTIVE;
        size=sizeof(*r)+51*4;break;
    }
    case IWX_WIDE_ID(1,IWX_ADD_STA): result[0]=IWX_ADD_STA_SUCCESS;break;
    case IWX_WIDE_ID(IWX_DATA_PATH_GROUP,IWX_RLC_CONFIG_CMD): assert(h->version==2);break;
    case IWX_WIDE_ID(1,IWX_MAC_CONTEXT_CMD): {
        const struct iwx_mac_ctx_cmd *m=(const void *)(wire+8);
        assert(h->length==sizeof(*m));assert(m->filter_flags&IWX_MAC_FILTER_DIS_DECRYPT);
        assert(m->filter_flags&IWX_MAC_FILTER_DIS_GRP_DECRYPT);break;
    }
    case IWX_WIDE_ID(IWX_DATA_PATH_GROUP,IWX_SCD_QUEUE_CONFIG_CMD): {
        const struct iwx_scd_queue_cfg_cmd *q=(const void *)(wire+8);
        struct iwx_tx_queue_cfg_rsp *r=(void *)result;r->queue_number=1;size=sizeof(*r);
        if(q->operation==IWX_SCD_QUEUE_ADD) {
            assert(q->u.add.cb_size==5 && q->u.add.sta_mask==1 && q->u.add.tid==IWX_MGMT_TID);
            assert(dma(q->u.add.tfdq_dram_addr)==ax.tx_desc && dma(q->u.add.bc_dram_addr)==&ax.tx_bc);
        }
        break;
    }
    default: break;
    }
    emit(id,index,0,result,size);
    if(id==IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP,IWX_NVM_ACCESS_COMPLETE)) emit(IWX_INIT_COMPLETE_NOTIF,0,0x80,0,0);
    if(id==IWX_WIDE_ID(1,IWX_SCAN_REQ_UMAC)) {
        const struct iwx_scan_req_umac_v14 *s=(const void *)(wire+8);
        assert(s->scan_params.general_params.flags&IWX_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE);
        assert(s->scan_params.channel_params.count==1 && s->scan_params.channel_params.channel_config[0].v2.channel_num==1);
        ap_beacon();emit(IWX_SCAN_COMPLETE_UMAC,0,0x80,0,0);
    }
    if(id==IWX_WIDE_ID(1,IWX_SCAN_ABORT_UMAC)) emit(IWX_SCAN_COMPLETE_UMAC,0,0x80,0,0);
}
static uint32_t model_read(volatile uint8_t *base,unsigned reg) {
    assert(base==(void *)registers && reg<sizeof registers && !(reg&3));
    if(reg==IWX_CSR_GP_CNTRL) return registers[reg/4]|1|(radio_blocked?0:IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW);
    if(reg==IWX_HBUS_TARG_PRPH_RDAT) return 0;
    return registers[reg/4];
}
static void model_write(volatile uint8_t *base,unsigned reg,uint32_t value) {
    assert(base==(void *)registers && reg<sizeof registers && !(reg&3));
    if(reg==IWX_CSR_INT || reg==IWX_CSR_FH_INT_STATUS) {registers[reg/4]&=~value;return;}
    registers[reg/4]=value;
    if(reg==IWX_CSR_RESET && (value&IWX_CSR_RESET_REG_FLAG_STOP_MASTER)) registers[reg/4]|=IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED;
    if(reg==IWX_HBUS_TARG_PRPH_WDAT && registers[IWX_HBUS_TARG_PRPH_WADDR/4]==((IWX_UREG_CPU_INIT_RUN&0xfffff)|(3U<<24))) {
        assert(value==1);assert(dma(ax.context.rbd_cfg.free_rbd_addr)==(void *)ax.rx_free);
        assert(dma(ax.context.rbd_cfg.used_rbd_addr)==(void *)ax.rx_used);
        assert(dma(ax.context.hcmd_cfg.cmd_queue_addr)==ax.cmd_desc && ax.context.hcmd_cfg.cmd_queue_size==5);
        for(unsigned group=0;group<3;group++) for(unsigned i=0;i<ax.fw.count[group];i++) {
            uint64_t address=group==0?ax.context.dram.lmac_img[i]:group==1?ax.context.dram.umac_img[i]:ax.context.dram.virtual_img[i];
            assert(!(address&4095));assert(!memcmp(dma(address),ax.fw.sections[group][i].data,ax.fw.sections[group][i].length));
        }
        assert(registers[IWX_CSR_LTR_LONG_VAL_AD/4]==0x82fa82fa);kicked=1;registers[IWX_CSR_INT/4]|=IWX_CSR_INT_BIT_ALIVE;
    }
    if(reg==IWX_RFH_Q0_FRBDCB_WIDX_TRG) {
        assert(kicked);
        if(!announced) {
            /* Device can DMA immediately after publishing the initial RBDs. */
            announced=1;struct iwx_alive_resp_v6 alive={.status=IWX_ALIVE_STATUS_OK};
            emit(IWX_ALIVE,0,0x80,&alive,sizeof alive);
        }
    }
    if(reg==IWX_HBUS_TARG_WRPTR) firmware_command(value);
}
static int tune_adapter(void *arg,uint8_t c) {(void)arg;return ax200_scan_channel(c);}
static int prepare_adapter(void *arg,const struct wifi_bss *b) {(void)arg;return ax200_prepare_bss(b);}
static int associate_adapter(void *arg,const struct wifi_bss *b) {(void)arg;return ax200_associate_bss(b);}
static int tx_adapter(void *arg,const uint8_t *p,unsigned n) {(void)arg;return ax200_tx(p,n);}
static int nonce(void *arg,uint8_t *p,unsigned n) {(void)arg;for(unsigned i=0;i<n;i++) p[i]=i;return 0;}
static void deliver(void *arg,const uint8_t *p,unsigned n) {(void)arg;assert(n<=sizeof ether_frame);memcpy(ether_frame,p,n);ether_length=n;deliveries++;}
static void port_state(void *arg,int up) {(void)arg;port_up=up;}
static const struct wifi_radio_ops radio_ops={tune_adapter,prepare_adapter,associate_adapter,tx_adapter,nonce,deliver,port_state};
static void drain(void) {
    uint8_t frame[WIFI_MPDU_MAX],channel;int n;ax200_poll();
    while((n=ax200_receive(frame,sizeof frame,&channel))>0) wifi_station_rx(&client,frame,n,channel,clock_us/1000);
}
static void reset_model(void) {
    ax_storage=0;memset(&probe_status,0,sizeof probe_status);memset(&model_device,0,sizeof model_device);
    memset(registers,0,sizeof registers);memset(&client,0,sizeof client);
    produced=commands=transmits=deliveries=port_up=0;clock_us=0;kicked=announced=drop_reply=0;
    bad_firmware=absent=radio_blocked=0;pci_command=0;
    registers[0x388/4]=0x00112233;registers[0x38c/4]=0x4455;registers[IWX_CSR_HW_REV/4]=0x340;
}
int main(void) {
    FILE *f=fopen("system/firmware/intel/iwlwifi-cc-a0-77.ucode","rb");assert(f);
    blob_size=fread(blob,1,sizeof blob,f);assert(blob_size==1368100);assert(!fclose(f));
    reset_model();absent=1;ax200_probe();assert(ax200_get_status()->state==AX200_ABSENT && !pci_command && !ax_storage);
    reset_model();bad_firmware=1;ax200_probe();assert(ax.status.state==AX200_BAD_FIRMWARE && !kicked && !pci_command);
    reset_model();ax200_probe();assert(ax.status.state==AX200_RADIO_READY && ax.running);
    assert(!memcmp(ax.status.mac,vector_sta,6) && (pci_command&6)==6 && commands>=10);
    assert(!wifi_station_init(&client,&radio_ops,0,vector_sta));uint8_t channel=1;
    assert(!wifi_station_join(&client,(const uint8_t *)"IEEE",4,vector_pmk,&channel,1,clock_us/1000));
    for(unsigned i=0;i<20 && client.state!=WIFI_STA_UP;i++) drain();
    assert(client.state==WIFI_STA_UP && port_up && transmits==4 && ax.tx_active);
    mpdu(vector_ccmp,sizeof vector_ccmp);drain();assert(deliveries==1 && ether_length==sizeof vector_plain-18);
    uint8_t eth[64]={0};memcpy(eth,vector_ap,6);memcpy(eth+6,vector_sta,6);eth[12]=8;
    for(unsigned i=0;i<1100;i++) assert(!wifi_station_tx(&client,eth,sizeof eth));
    assert(produced>1024 && ax.tx_next==(1104&255) && client.pair.tx_pn==1100);
    struct wifi_ccmp_key peer={0};uint8_t plain[WIFI_MPDU_MAX];assert(!wifi_ccmp_install(&peer,vector_ptk+32,0,0));
    assert(wifi_ccmp_open(&peer,last_frame,last_length,plain,sizeof plain)==sizeof eth+18);
    assert(!memcmp(plain+32,eth+14,sizeof eth-14));
    wifi_station_disconnect(&client);ax200_leave();assert(!port_up && !ax.tx_active && !ax.mac_active && !ax.phy_active);
    drop_reply=1;uint32_t data=0;assert(command(1,IWX_POWER_TABLE_CMD,&data,sizeof data)==-1);
    assert(ax.status.state==AX200_STOPPED && !(pci_command&4) && !ax.running);
    puts("AX200 model: pinned firmware/context DMA, NVM/regulatory init, scan/auth/association/WPA2, encrypted Ethernet, 1100 TX and RX ring wraps, teardown and bus-master fail-stop passed (physical hardware untested)");
}
