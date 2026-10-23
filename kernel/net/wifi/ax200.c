/* AX200 PCIe transport. The hardware sequencing and wire definitions derive
 * from OpenBSD iwx (if_iwx.c rev 1.230, if_iwxreg.h rev 1.75); see
 * third_party/iwx/LICENSE for all original copyright/permission notices.
 * Uses the BSD alternative of the Intel dual license.
 *
 * One adapter, polling, a 512-entry RX ring and a 256-entry command queue.
 * DMA storage has permanent lifetime, including after device timeout. A fatal
 * error disables PCI bus mastering before returning control to the caller.
 * No RX packet can recursively invoke a firmware command from a DMA callback.
 */
#include "ax200.h"
#include "ax200_protocol.h"
#include "ax200_regs.h"
#include "drivers/bus/pci.h"
#include "drivers/timer/timer.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "fs/vfs.h"
#include "crypto/sha256.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#define RX_COUNT 512
#define CMD_COUNT 256
#define FW_ARENA_SIZE (2U*1024*1024)
#define FW_PATH "/system/firmware/intel/iwlwifi-cc-a0-77.ucode"
static const uint8_t firmware_sha256[32]={
    0x18,0x66,0xa5,0x8a,0x1d,0xae,0xe5,0x6b,0x12,0x03,0x62,0x12,0x2d,0x5f,0x75,0x48,
    0xb4,0x43,0xb5,0x3d,0x33,0xf2,0x55,0x8a,0x7e,0x44,0x07,0xd5,0x8f,0xd2,0x7d,0xab
};
static struct ax200_device {
    struct pci_device pci;
    volatile uint8_t *regs;
    struct ax200_status status;
    struct ax200_fw fw;
    uint8_t raw_fw[AX200_FW_MAX];
    uint8_t arena[FW_ARENA_SIZE] __attribute__((aligned(4096)));
    struct iwx_context_info context __attribute__((aligned(4096)));
    volatile uint64_t rx_free[RX_COUNT] __attribute__((aligned(4096)));
    volatile uint32_t rx_used[RX_COUNT] __attribute__((aligned(4096)));
    volatile struct iwx_rb_status rx_status __attribute__((aligned(4096)));
    uint8_t rx[RX_COUNT][4096] __attribute__((aligned(4096)));
    struct iwx_tfh_tfd cmd_desc[CMD_COUNT] __attribute__((aligned(4096)));
    uint8_t cmd[CMD_COUNT][4096] __attribute__((aligned(4096)));
    struct iwx_tfh_tfd tx_desc[CMD_COUNT] __attribute__((aligned(4096)));
    struct iwx_agn_scd_bc_tbl tx_bc __attribute__((aligned(4096)));
    uint8_t tx[CMD_COUNT][2048] __attribute__((aligned(4096)));
    struct {uint8_t data[WIFI_MPDU_MAX],channel;unsigned length;uint32_t gp2;} frames[32];
    unsigned frame_head,frame_tail,tx_next,tx_qid;
    uint32_t rx_gp2,beacon_gp2,tx_result;
    uint32_t rate;
    uint32_t mac_sku;
    uint8_t scanning,phy_active,mac_active,binding_active,sta_active,tx_active,tx_pending,tx_completed;
    uint8_t tx_index,channel;
    struct wifi_bss bss;
    uint8_t response[4096];
    unsigned rx_next, cmd_next, response_len;
    uint16_t pending_id;
    uint8_t pending_idx, pending, completed, command_failed, alive, init_complete, running,rx_started;
} *ax_storage;
static struct ax200_status probe_status;
#define ax (*ax_storage)
_Static_assert(sizeof(struct iwx_tfh_tfd)==256,"AX200 TFD wire size");
_Static_assert(sizeof(struct iwx_rb_status)==12,"AX200 RX status wire size");
#ifndef AX200_MMIO_READ
#define AX200_MMIO_READ(base,reg) (*(volatile uint32_t *)((base)+(reg)))
#define AX200_MMIO_WRITE(base,reg,value) (*(volatile uint32_t *)((base)+(reg))=(value))
#endif
static uint32_t rd(unsigned reg) {return AX200_MMIO_READ(ax.regs,reg);}
static void wr(unsigned reg,uint32_t value) {AX200_MMIO_WRITE(ax.regs,reg,value);}
static void set(unsigned reg,uint32_t bits) {wr(reg,rd(reg)|bits);}
static void clear(unsigned reg,uint32_t bits) {wr(reg,rd(reg)&~bits);}
static void delay(unsigned us) {
    uint64_t start=time_get_us();while(time_get_us()-start<us) __asm__ volatile("pause");
}
static int wait_bits(unsigned reg,uint32_t mask,uint32_t value,unsigned us) {
    uint64_t start=time_get_us();
    do {if((rd(reg)&mask)==value) return 0;__asm__ volatile("pause");} while(time_get_us()-start<us);
    return -1;
}
static void publish(void) {__asm__ volatile("mfence" ::: "memory");}
static uint32_t prph_read(uint32_t addr) {
    wr(IWX_HBUS_TARG_PRPH_RADDR,(addr&0xfffff)|(3U<<24));(void)rd(IWX_HBUS_TARG_PRPH_RADDR);
    return rd(IWX_HBUS_TARG_PRPH_RDAT);
}
static void prph_write(uint32_t addr,uint32_t value) {
    wr(IWX_HBUS_TARG_PRPH_WADDR,(addr&0xfffff)|(3U<<24));(void)rd(IWX_HBUS_TARG_PRPH_WADDR);
    wr(IWX_HBUS_TARG_PRPH_WDAT,value);
}
static int nic_lock(void) {
    set(IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);delay(2);
    return wait_bits(IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY|
        IWX_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP,IWX_CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN,150000);
}
static void nic_unlock(void) {clear(IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);}
static void stop(uint32_t reason) {
    ax.running=0;ax.pending=0;ax.status.last_error=reason;ax.status.state=AX200_STOPPED;
    wr(IWX_CSR_INT_MASK,0);set(IWX_CSR_RESET,IWX_CSR_RESET_REG_FLAG_STOP_MASTER);
    (void)wait_bits(IWX_CSR_RESET,IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED,IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED,10000);
    uint16_t cmd=pci_read16(ax.pci.bus,ax.pci.device,ax.pci.function,PCI_COMMAND);
    pci_write16(ax.pci.bus,ax.pci.device,ax.pci.function,PCI_COMMAND,cmd&~4U);
    set(IWX_CSR_RESET,IWX_CSR_RESET_REG_FLAG_SW_RESET);
    kprintf("ax200: transport stopped (error %08x); link remains down\n",reason);
}
static void receive_mpdu(const uint8_t *p,unsigned n) {
    if(n<IWX_RX_DESC_SIZE_V1) return;
    const struct iwx_rx_mpdu_desc *d=(const void *)p;unsigned len=d->mpdu_len;
    if((d->status&(IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK))!=
       (IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK) ||
       (d->mac_flags2&IWX_RX_MPDU_MFLG2_AMSDU) || len<24 || len>WIFI_MPDU_MAX || len>n-IWX_RX_DESC_SIZE_V1) return;
    unsigned next=(ax.frame_head+1)&31;if(next==ax.frame_tail) return;
    const uint8_t *frame=p+IWX_RX_DESC_SIZE_V1;
    memcpy(ax.frames[ax.frame_head].data,frame,len);
    if(d->mac_flags2&IWX_RX_MPDU_MFLG2_PAD) {
        unsigned hl=24;
        if((frame[0]&0x0c)==8 && (frame[0]&0x80)) hl+=2;
        if((frame[1]&3)==3) hl+=6;
        if(frame[1]&0x80) hl+=4;
        if(frame[1]&0x40) hl+=8;
        if(len<hl+2) return;
        memmove(ax.frames[ax.frame_head].data+hl,frame+hl+2,len-hl-2);len-=2;
    }
    ax.frames[ax.frame_head].length=len;ax.frames[ax.frame_head].channel=d->v1.channel;
    ax.frames[ax.frame_head].gp2=d->v1.gp2_on_air_rise;ax.frame_head=next;
}
static int record(void *arg,const uint8_t *p,unsigned n) {
    (void)arg;unsigned id=(unsigned)p[1]*256+p[0];
    if(id==IWX_ALIVE) {
        unsigned version=ax200_fw_command_version(&ax.fw,0,IWX_ALIVE,1);
        unsigned expected=version==6?sizeof(struct iwx_alive_resp_v6):
            version==5?sizeof(struct iwx_alive_resp_v5):sizeof(struct iwx_alive_resp_v4);
        if(n-4!=expected || p[4]!=0xfe || p[5]!=0xca) return -1;
        ax.alive=1;ax.status.state=AX200_FIRMWARE_ALIVE;
    } else if(id==IWX_INIT_COMPLETE_NOTIF) ax.init_complete=1;
    else if(id==IWX_REPLY_RX_MPDU_CMD) receive_mpdu(p+4,n-4);
    else if(id==IWX_SCAN_COMPLETE_UMAC) ax.scanning=0;
    else if(id==IWX_TX_CMD && ax.tx_pending && p[3]==ax.tx_qid) {
        if(n-4<sizeof(struct iwx_tx_resp)+4) return -1;
        const struct iwx_tx_resp *r=(const void *)(p+4);
        uint32_t ssn;memcpy(&ssn,(const uint8_t *)&r->status+sizeof r->status,4);
        if(r->frame_count!=1 || ssn!=ax.tx_next) return -1;
        ax.tx_result=r->status.status&IWX_TX_STATUS_MSK;ax.tx_completed=1;
    }
    if(ax.pending && !(p[3]&0x80) && (p[3]&0x7f)==0 && p[2]==ax.pending_idx) {
        if((id&~(IWX_CMD_FAILED_MSK<<8))!=ax.pending_id || n-4>sizeof ax.response) return -1;
        ax.command_failed=(p[1]&IWX_CMD_FAILED_MSK)!=0;ax.response_len=n-4;
        memcpy(ax.response,p+4,n-4);ax.completed=1;
    }
    return 0;
}
void ax200_poll(void) {
    if(!ax_storage || !ax.running) return;
    uint32_t cause=rd(IWX_CSR_INT);
    if(cause==UINT32_MAX || (cause&0xfffffff0)==0xa5a5a5a0) {stop(0x10001);return;}
    wr(IWX_CSR_INT,cause);uint32_t fh=rd(IWX_CSR_FH_INT_STATUS);wr(IWX_CSR_FH_INT_STATUS,fh);
    if((cause&(IWX_CSR_INT_BIT_HW_ERR|IWX_CSR_INT_BIT_SW_ERR|IWX_CSR_INT_BIT_CT_KILL)) ||
       (fh&IWX_CSR_FH_INT_BIT_ERR)) {stop(cause|fh);return;}
    ax.status.rfkill=!(rd(IWX_CSR_GP_CNTRL)&IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW);
    if(cause&IWX_CSR_INT_BIT_ALIVE) {
        /* Firmware has configured RFH and can now consume the free list. */
        for(unsigned i=0;i<RX_COUNT;i++) ax.rx_free[i]=V2P(ax.rx[i])|i;
        publish();wr(IWX_RFH_Q0_FRBDCB_WIDX_TRG,8);ax.rx_started=1;
    }
    if(!ax.rx_started) return;
    publish();unsigned closed=ax.rx_status.closed_rb_num&(RX_COUNT-1);
    unsigned budget=RX_COUNT;
    while(ax.rx_next!=closed && budget--) {
        unsigned slot=ax.rx_next;
        /* AX200's default queue preserves RBD order. Like iwx, replenish the
         * buffer in the same free-list slot after processing its records. */
        if(ax200_rx_records(ax.rx[slot],4096,record,0)) {stop(0x10003);return;}
        memset(ax.rx[slot],0,4096);ax.rx_free[slot]=V2P(ax.rx[slot])|slot;
        ax.rx_next=(slot+1)&(RX_COUNT-1);
    }
    publish();unsigned release=closed?closed-1:RX_COUNT-1;
    wr(IWX_RFH_Q0_FRBDCB_WIDX_TRG,release&~7U);
}
static int command_version(unsigned group,unsigned opcode,unsigned version,const void *data,unsigned n) {
    if(!ax.running || ax.pending || n>4088) return -1;
    if(!group) group=1; /* API >=50 rejects narrow legacy group commands */
    unsigned i=ax.cmd_next;uint8_t *p=ax.cmd[i];memset(p,0,8+n);
    p[0]=opcode;p[1]=group;p[2]=i;p[4]=n;p[5]=n>>8;
    p[7]=version;
    memcpy(p+8,data,n);struct iwx_tfh_tfd *t=&ax.cmd_desc[i];memset(t,0,sizeof(*t));
    unsigned first=n+8<20?n+8:20;t->tbs[0].tb_len=first;t->tbs[0].addr=V2P(p);t->num_tbs=1;
    if(n+8>first) {t->tbs[1].tb_len=n+8-first;t->tbs[1].addr=V2P(p+first);t->num_tbs=2;}
    ax.pending=1;ax.pending_idx=i;ax.pending_id=(group<<8)|opcode;ax.completed=0;ax.command_failed=0;
    ax.cmd_next=(i+1)&255;publish();wr(IWX_HBUS_TARG_WRPTR,ax.cmd_next);
    uint64_t start=time_get_us();
    while(ax.running && !ax.completed && time_get_us()-start<1000000) {ax200_poll();delay(20);}
    if(!ax.running) return -1;
    if(!ax.completed) {stop(0x20000|ax.pending_id);return -1;}
    ax.pending=0;
    if(ax.command_failed) {stop(0x30000|ax.pending_id);return -1;}
    return 0;
}
static int command(unsigned group,unsigned opcode,const void *data,unsigned n) {
    return command_version(group,opcode,0,data,n);
}
static int hardware_ready(void) {
    set(IWX_CSR_HW_IF_CONFIG_REG,IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
    if(!wait_bits(IWX_CSR_HW_IF_CONFIG_REG,IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
        IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,50)) {
        set(IWX_CSR_MBOX_SET_REG,IWX_CSR_MBOX_SET_REG_OS_ALIVE);return 0;
    }
    set(IWX_CSR_DBG_LINK_PWR_MGMT_REG,IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);delay(1000);
    for(unsigned i=0;i<10;i++) {
        set(IWX_CSR_HW_IF_CONFIG_REG,IWX_CSR_HW_IF_CONFIG_REG_PREPARE);
        uint64_t start=time_get_us();
        do {
            set(IWX_CSR_HW_IF_CONFIG_REG,IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
            if(!wait_bits(IWX_CSR_HW_IF_CONFIG_REG,IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
                IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,50)) {
                set(IWX_CSR_MBOX_SET_REG,IWX_CSR_MBOX_SET_REG_OS_ALIVE);return 0;
            }
            delay(200);
        } while(time_get_us()-start<150000);
        delay(25000);
    }
    return -1;
}
static int prepare_hardware(void) {
    if(hardware_ready()) return -1;
    uint32_t hpm=prph_read(IWX_HPM_DEBUG);
    if(hpm!=0xa5a5a5a0 && (hpm&IWX_PERSISTENCE_BIT)) {
        if(prph_read(IWX_PREG_PRPH_WPROT_22000)&IWX_PREG_WFPM_ACCESS) return -1;
        prph_write(IWX_HPM_DEBUG,hpm&~IWX_PERSISTENCE_BIT);
    }
    set(IWX_CSR_RESET,IWX_CSR_RESET_REG_FLAG_SW_RESET);delay(5000);
    wr(IWX_CSR_INT_MASK,0);wr(IWX_CSR_INT,UINT32_MAX);
    set(IWX_CSR_GIO_CHICKEN_BITS,IWX_CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);
    set(IWX_CSR_DBG_HPET_MEM_REG,IWX_CSR_DBG_HPET_MEM_REG_VAL);
    set(IWX_CSR_HW_IF_CONFIG_REG,IWX_CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);
    set(IWX_CSR_GIO_REG,IWX_CSR_GIO_REG_VAL_L0S_DISABLED);
    set(IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    if(wait_bits(IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
        IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,25000)) return -1;
    uint32_t phy=ax.fw.phy,rev=ax.status.hardware_revision;
    rev=(rev&0xfff0)|(IWX_CSR_HW_REV_STEP(rev<<2)<<2);
    uint32_t mask=IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH|IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP|
        IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP|IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH|
        IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE|IWX_CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI|IWX_CSR_HW_IF_CONFIG_REG_BIT_MAC_SI;
    uint32_t cfg=(IWX_CSR_HW_REV_STEP(rev)<<IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_STEP)|
        (IWX_CSR_HW_REV_DASH(rev)<<IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_DASH)|
        (((phy&IWX_FW_PHY_CFG_RADIO_TYPE)>>IWX_FW_PHY_CFG_RADIO_TYPE_POS)<<IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE)|
        (((phy&IWX_FW_PHY_CFG_RADIO_STEP)>>IWX_FW_PHY_CFG_RADIO_STEP_POS)<<IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_STEP)|
        (((phy&IWX_FW_PHY_CFG_RADIO_DASH)>>IWX_FW_PHY_CFG_RADIO_DASH_POS)<<IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_DASH);
    wr(IWX_CSR_HW_IF_CONFIG_REG,(rd(IWX_CSR_HW_IF_CONFIG_REG)&~mask)|cfg);
    wr(IWX_CSR_UCODE_DRV_GP1_CLR,IWX_CSR_UCODE_SW_BIT_RFKILL|IWX_CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
    *(volatile uint8_t *)(ax.regs+IWX_CSR_INT_COALESCING)=IWX_HOST_INT_TIMEOUT_DEF;
    set(IWX_CSR_MAC_SHADOW_REG_CTRL,0x800fffff);return 0;
}
static int load_context(void) {
    memset(&ax.context,0,sizeof ax.context);unsigned used=0;
    ax.context.version.mac_id=ax.status.hardware_revision;ax.context.version.size=sizeof ax.context/4;
    ax.context.control.control_flags=IWX_CTXT_INFO_TFD_FORMAT_LONG|(9U<<IWX_CTXT_INFO_RB_CB_SIZE_POS)|
        (IWX_CTXT_INFO_RB_SIZE_4K<<IWX_CTXT_INFO_RB_SIZE_POS);
    ax.context.rbd_cfg.free_rbd_addr=V2P(ax.rx_free);ax.context.rbd_cfg.used_rbd_addr=V2P(ax.rx_used);
    ax.context.rbd_cfg.status_wr_ptr=V2P(&ax.rx_status);
    ax.context.hcmd_cfg.cmd_queue_addr=V2P(ax.cmd_desc);ax.context.hcmd_cfg.cmd_queue_size=5; /* log2(256)-3 */
    for(unsigned group=0;group<3;group++) for(unsigned i=0;i<ax.fw.count[group];i++) {
        const struct ax200_fw_section *s=&ax.fw.sections[group][i];unsigned padded=(s->length+4095)&~4095U;
        if(padded>sizeof ax.arena-used) return -1;
        memcpy(ax.arena+used,s->data,s->length);uint64_t address=V2P(ax.arena+used);used+=padded;
        /* Avoid taking an address of a packed member. */
        if(group==0) ax.context.dram.lmac_img[i]=address;
        else if(group==1) ax.context.dram.umac_img[i]=address;
        else ax.context.dram.virtual_img[i]=address;
    }
    uint64_t base=V2P(&ax.context);publish();wr(IWX_CSR_CTXT_INFO_BA,base);wr(IWX_CSR_CTXT_INFO_BA+4,base>>32);
    if(nic_lock()) return -1;
    uint32_t ltr=IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_REQ|
        (IWX_CSR_LTR_LONG_VAL_AD_SCALE_USEC<<IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_SCALE_SHIFT)|
        (250U<<IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_VAL_SHIFT)|IWX_CSR_LTR_LONG_VAL_AD_SNOOP_REQ|
        (IWX_CSR_LTR_LONG_VAL_AD_SCALE_USEC<<IWX_CSR_LTR_LONG_VAL_AD_SNOOP_SCALE_SHIFT)|250;
    wr(IWX_CSR_LTR_LONG_VAL_AD,ltr);prph_write(IWX_UREG_CPU_INIT_RUN,1);nic_unlock();
    return 0;
}
static int get_nvm(void) {
    struct iwx_init_extended_cfg_cmd init={.init_flags=IWX_INIT_NVM};
    struct iwx_nvm_access_complete_cmd complete={0};struct iwx_nvm_get_info request={0};
    if(command(IWX_SYSTEM_GROUP,IWX_INIT_EXTENDED_CFG_CMD,&init,sizeof init) ||
       command(IWX_REGULATORY_AND_NVM_GROUP,IWX_NVM_ACCESS_COMPLETE,&complete,sizeof complete)) return -1;
    uint64_t start=time_get_us();
    while(ax.running && !ax.init_complete && time_get_us()-start<2000000) {ax200_poll();delay(20);}
    if(!ax.running || !ax.init_complete || command(IWX_REGULATORY_AND_NVM_GROUP,IWX_NVM_GET_INFO,&request,sizeof request)) return -1;
    if(ax.response_len!=sizeof(struct iwx_nvm_get_info_rsp) ||
       ax200_fw_command_version(&ax.fw,IWX_REGULATORY_AND_NVM_GROUP,IWX_NVM_GET_INFO,1)!=4) return -1;
    struct iwx_nvm_get_info_rsp nvm;memcpy(&nvm,ax.response,sizeof nvm);
    if(nvm.general.flags&IWX_NVM_GENERAL_FLAGS_EMPTY_OTP || !nvm.general.n_hw_addrs ||
       !nvm.phy_sku.tx_chains || !nvm.phy_sku.rx_chains || nvm.regulatory.n_channels>IWX_NUM_CHANNELS) return -1;
    ax.status.tx_ant=nvm.phy_sku.tx_chains&3;ax.status.rx_ant=nvm.phy_sku.rx_chains&3;
    ax.mac_sku=nvm.mac_sku.mac_sku_flags;
    if(!ax.status.tx_ant || !ax.status.rx_ant) return -1;
    for(unsigned i=0;i<51 && i<nvm.regulatory.n_channels;i++) {
        uint32_t band=i<14?IWX_NVM_MAC_SKU_FLAGS_BAND_2_4_ENABLED:IWX_NVM_MAC_SKU_FLAGS_BAND_5_2_ENABLED;
        ax.status.channel_flags[i]=(nvm.mac_sku.mac_sku_flags&band)?nvm.regulatory.channel_profile[i]:0;
    }
    if(nic_lock()) return -1;
    for(unsigned source=0;source<2;source++) {
        unsigned offset=source?0x380:0x388;uint32_t lo=rd(offset),hi=rd(offset+4);
        ax.status.mac[0]=lo>>24;ax.status.mac[1]=lo>>16;ax.status.mac[2]=lo>>8;ax.status.mac[3]=lo;
        ax.status.mac[4]=hi>>8;ax.status.mac[5]=hi;
        if(!(ax.status.mac[0]&1) && (lo|hi) && memcmp(ax.status.mac,"\x02\xcc\xaa\xff\xee\0",6)) break;
    }
    nic_unlock();
    unsigned bits=0;for(unsigned i=0;i<6;i++) bits|=ax.status.mac[i];
    if(!bits || (ax.status.mac[0]&1) || !memcmp(ax.status.mac,"\x02\xcc\xaa\xff\xee\0",6)) return -1;
    ax.status.state=AX200_NVM_READY;return 0;
}
static int firmware_cap(unsigned bit) {return bit<256 && (ax.fw.capabilities[bit/32]&(1U<<(bit%32)));}
static int firmware_api(unsigned bit) {return bit<128 && (ax.fw.api[bit/32]&(1U<<(bit%32)));}
static uint32_t response_status(void) {uint32_t result=UINT32_MAX;if(ax.response_len==4) memcpy(&result,ax.response,4);return result;}
static int radio_init(void) {
    struct iwx_tx_ant_cfg_cmd ant={.valid=ax.status.tx_ant};
    struct iwx_bt_coex_cmd bt={.mode=IWX_BT_COEX_WIFI};
    struct iwx_soc_configuration_cmd soc={.flags=IWX_SOC_CONFIG_CMD_FLAGS_DISCRETE};
    struct iwx_dqa_enable_cmd dqa={.cmd_queue=0};struct iwx_device_power_cmd power={0};
    struct iwx_beacon_filter_cmd filter={0};
    struct iwx_scan_config scan={.tx_chains=ax.status.tx_ant,.rx_chains=ax.status.rx_ant};
    if(!firmware_api(IWX_UCODE_TLV_API_REDUCED_SCAN_CONFIG) ||
       ax200_fw_command_version(&ax.fw,1,IWX_SCAN_REQ_UMAC,0)!=15 ||
       ax200_fw_command_version(&ax.fw,IWX_DATA_PATH_GROUP,IWX_SCD_QUEUE_CONFIG_CMD,0)!=3) return -1;
    if(command(0,IWX_TX_ANT_CONFIGURATION_CMD,&ant,sizeof ant) || command(0,IWX_BT_CONFIG,&bt,sizeof bt) ||
       command(IWX_SYSTEM_GROUP,IWX_SOC_CONFIGURATION_CMD,&soc,sizeof soc) ||
       command(IWX_DATA_PATH_GROUP,IWX_DQA_ENABLE_CMD,&dqa,sizeof dqa) ||
       command(0,IWX_POWER_TABLE_CMD,&power,sizeof power)) return -1;
    if(firmware_cap(IWX_UCODE_TLV_CAPA_CT_KILL_BY_FW)) {
        struct iwx_temp_report_ths_cmd temperature={0};
        if(command(IWX_PHY_OPS_GROUP,IWX_TEMP_REPORTING_THRESHOLDS_CMD,&temperature,sizeof temperature)) return -1;
    }
    /* Ask firmware for its current/default geographic profile. No country is
     * invented from the development machine or forced by the OS. */
    struct iwx_mcc_update_cmd mcc={.mcc=('Z'<<8)|'Z',.source_id=IWX_MCC_SOURCE_GET_CURRENT};
    if(command(0,IWX_MCC_UPDATE_CMD,&mcc,sizeof mcc)) return -1;
    unsigned offset,count;
    if(firmware_cap(IWX_UCODE_TLV_CAPA_MCC_UPDATE_11AX_SUPPORT)) {
        offset=sizeof(struct iwx_mcc_update_resp_v4);
        if(ax.response_len<offset) return -1;
        struct iwx_mcc_update_resp_v4 *r=(void *)ax.response;count=r->n_channels;
    } else {
        offset=sizeof(struct iwx_mcc_update_resp);
        if(ax.response_len<offset) return -1;
        struct iwx_mcc_update_resp *r=(void *)ax.response;count=r->n_channels;
    }
    if(count>110 || ax.response_len!=offset+count*4) return -1;
    for(unsigned i=0;i<51;i++) {
        uint32_t flags=0;if(i<count) memcpy(&flags,ax.response+offset+i*4,4);
        /* Firmware combines its locale with NVM restrictions. Its current
         * profile can allow active use of a previously passive channel; the
         * physical SKU's disabled bands still stay disabled. */
        uint32_t band=i<14?IWX_NVM_MAC_SKU_FLAGS_BAND_2_4_ENABLED:IWX_NVM_MAC_SKU_FLAGS_BAND_5_2_ENABLED;
        ax.status.channel_flags[i]=(ax.mac_sku&band)?flags:0;
    }
    if(command(0,IWX_REPLY_BEACON_FILTERING_CMD,&filter,sizeof filter) ||
       command(1,IWX_SCAN_CFG_CMD,&scan,sizeof scan)) return -1;
    ax.status.state=AX200_RADIO_READY;return 0;
}
void ax200_probe(void) {
    if(ax_storage || probe_status.state!=AX200_ABSENT || !tsc_get_freq_hz()) return;
    const struct pci_device *found=0;
    for(unsigned i=0;i<pci_devices_count();i++) {
        const struct pci_device *p=pci_get_device(i);
        if(p->vendor_id==0x8086 && p->device_id==0x2723 && p->class_code==2 && p->subclass==0x80) {found=p;break;}
    }
    if(!found) return;
    /* Allocate only when the adapter exists. Keeping multi-megabyte DMA
     * storage in kernel .bss made UEFI's fixed-address kernel reservation
     * span firmware-owned low memory even on machines without this card.
     * The run is physically contiguous and permanently retained on failure. */
    uint64_t physical=pmm_alloc_contiguous((sizeof(*ax_storage)+4095)/4096,1);
    if(!physical) {probe_status.state=AX200_STOPPED;probe_status.last_error=0x40007;return;}
    ax_storage=(struct ax200_device *)P2V(physical);memset(ax_storage,0,sizeof(*ax_storage));
    ax.pci=*found;struct vfs_stat st;
    if(vfs_stat(FW_PATH,&st)) {ax.status.state=AX200_MISSING_FIRMWARE;kprintf("ax200: missing " FW_PATH "; link down\n");return;}
    ax.status.state=AX200_BAD_FIRMWARE;
    if(st.size<88 || st.size>sizeof ax.raw_fw) {kprintf("ax200: invalid firmware size\n");return;}
    size_t got=0;uint8_t digest[32];
    if(vfs_read(FW_PATH,0,ax.raw_fw,st.size,&got) || got!=st.size) return;
    sha256(ax.raw_fw,got,digest);
    if(memcmp(digest,firmware_sha256,32) || ax200_fw_parse(&ax.fw,ax.raw_fw,got)) {
        kprintf("ax200: firmware integrity/API mismatch; link down\n");return;
    }
    struct pci_bar bar=pci_read_bar(found->bus,found->device,found->function,0);
    if(!bar.valid || !bar.is_mmio || bar.size<0x2000) {ax.status.state=AX200_STOPPED;return;}
    ax.regs=(volatile uint8_t *)vmm_map_mmio(bar.address,bar.size);if(!ax.regs) {ax.status.state=AX200_STOPPED;return;}
    /* D0 and memory/bus-master enable, INTx disabled: all causes are polled. */
    unsigned cap=pci_read8(found->bus,found->device,found->function,PCI_CAP_PTR)&~3U;
    for(unsigned tries=0;cap>=0x40 && cap<=0xfc && tries<48;tries++) {
        if(pci_read8(found->bus,found->device,found->function,cap)==1 && cap<=0xf8) {
            uint16_t pm=pci_read16(found->bus,found->device,found->function,cap+4);
            pci_write16(found->bus,found->device,found->function,cap+4,pm&~0x8003U);delay(10000);break;
        }
        cap=pci_read8(found->bus,found->device,found->function,cap+1)&~3U;
    }
    uint16_t cmd=pci_read16(found->bus,found->device,found->function,PCI_COMMAND);
    pci_write16(found->bus,found->device,found->function,PCI_COMMAND,cmd|6|(1U<<10));
    ax.status.state=AX200_STARTING;ax.status.hardware_revision=rd(IWX_CSR_HW_REV);
    if(prepare_hardware()) {stop(0x40001);return;}
    if(nic_lock()) {stop(0x40006);return;}
    prph_write(IWX_UREG_CHICK,IWX_UREG_CHICK_MSI_ENABLE);nic_unlock();
    set(IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_RFKILL_WAKE_L1A_EN);
    ax.running=1;
    if(load_context()) {stop(0x40002);return;}
    uint64_t start=time_get_us();
    while(ax.running && !ax.alive && time_get_us()-start<2000000) {ax200_poll();delay(20);}
    if(!ax.running) return;
    if(!ax.alive) {stop(0x40003);return;}
    if(get_nvm()) {if(ax.running) stop(0x40004);return;}
    if(radio_init()) {if(ax.running) stop(0x40005);return;}
    kprintf("ax200: API-77 firmware/NVM ready, radio %s; awaiting WPA2 association\n",ax.status.rfkill?"blocked":"enabled");
}
const struct ax200_status *ax200_get_status(void) {return ax_storage?&ax.status:&probe_status;}
static const uint8_t nvm_channels[51]={
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,36,40,44,48,52,56,60,64,
    68,72,76,80,84,88,92,96,100,104,108,112,116,120,124,128,132,136,
    140,144,149,153,157,161,165,169,173,177,181
};
static uint32_t channel_flags(uint8_t channel) {
    for(unsigned i=0;i<51;i++) if(nvm_channels[i]==channel) return ax.status.channel_flags[i];
    return 0;
}
int ax200_channels(uint8_t *out,unsigned cap) {
    unsigned n=0;if(!ax_storage || !out || ax.status.state!=AX200_RADIO_READY || !ax.running) return 0;
    for(unsigned i=0;i<51 && n<cap;i++) if(ax.status.channel_flags[i]&IWX_NVM_CHANNEL_VALID)
        out[n++]=nvm_channels[i];
    return n;
}
int ax200_receive(uint8_t *out,unsigned cap,uint8_t *channel) {
    if(!ax_storage || !out || !channel || ax.frame_tail==ax.frame_head || !ax.running) return 0;
    unsigned i=ax.frame_tail,n=ax.frames[i].length;ax.frame_tail=(i+1)&31;
    if(n>cap) return -1;
    memcpy(out,ax.frames[i].data,n);*channel=ax.frames[i].channel;ax.rx_gp2=ax.frames[i].gp2;return n;
}
static int scan_abort(void) {
    if(!ax.scanning) return 0;
    struct iwx_umac_scan_abort req={0};
    if(command(1,IWX_SCAN_ABORT_UMAC,&req,sizeof req)) return -1;
    uint64_t start=time_get_us();
    while(ax.running && ax.scanning && time_get_us()-start<1000000) {ax200_poll();delay(20);}
    if(ax.scanning) {if(ax.running) stop(0x50001);return -1;}return 0;
}
int ax200_scan_channel(uint8_t channel) {
    if(!ax_storage || !ax.running || ax.status.state!=AX200_RADIO_READY || ax.status.rfkill ||
       !(channel_flags(channel)&IWX_NVM_CHANNEL_VALID) || ax.mac_active || scan_abort()) return -1;
    struct iwx_scan_req_umac_v14 scan={0};scan.ooc_priority=IWX_SCAN_PRIORITY_EXT_6;
    struct iwx_scan_general_params_v10 *gp=&scan.scan_params.general_params;
    gp->flags=IWX_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE|IWX_UMAC_SCAN_GEN_FLAGS_V2_PASS_ALL|
        IWX_UMAC_SCAN_GEN_FLAGS_V2_NTFY_ITER_COMPLETE;
    gp->scan_priority=IWX_SCAN_PRIORITY_EXT_6;
    for(unsigned i=0;i<2;i++) {gp->active_dwell[i]=10;gp->passive_dwell[i]=110;}
    scan.scan_params.periodic_params.schedule[0].iter_count=1;
    struct iwx_scan_channel_params_v6 *cp=&scan.scan_params.channel_params;
    cp->flags=IWX_SCAN_CHANNEL_FLAG_ENABLE_CHAN_ORDER;cp->count=1;
    if(firmware_api(IWX_UCODE_TLV_API_SCAN_EXT_CHAN_VER)) {
        cp->channel_config[0].v2.channel_num=channel;
        cp->channel_config[0].v2.band=channel<=14?IWX_PHY_BAND_24:IWX_PHY_BAND_5;
        cp->channel_config[0].v2.iter_count=1;
    } else {cp->channel_config[0].v1.channel_num=channel;cp->channel_config[0].v1.iter_count=1;}
    /* Even passive requests contain a syntactically valid probe template. The
     * FORCE_PASSIVE flag and zero SSID bitmap prevent probe transmission. */
    struct iwx_scan_probe_req *pr=&scan.scan_params.probe_params.preq;uint8_t *p=pr->buf;
    p[0]=0x40;memset(p+4,255,6);memcpy(p+10,ax.status.mac,6);memset(p+16,255,6);
    p[24]=0;p[25]=0;pr->mac_header.len=26;
    static const uint8_t rates24[]={1,8,2,4,11,22,12,18,24,36,50,4,48,72,96,108};
    static const uint8_t rates5[]={1,8,12,18,24,36,48,72,96,108};
    memcpy(p+26,rates24,sizeof rates24);pr->band_data[0].offset=26;pr->band_data[0].len=sizeof rates24;
    memcpy(p+26+sizeof rates24,rates5,sizeof rates5);pr->band_data[1].offset=26+sizeof rates24;pr->band_data[1].len=sizeof rates5;
    pr->common_data.offset=26+sizeof rates24+sizeof rates5;
    ax.channel=channel;ax.scanning=1;
    if(command(1,IWX_SCAN_REQ_UMAC,&scan,sizeof scan)) {ax.scanning=0;return -1;}
    return 0;
}
static int phy_context(unsigned action) {
    struct iwx_phy_context_cmd cmd={0};cmd.action=action;
    cmd.ci.band=ax.bss.channel<=14?IWX_PHY_BAND_24:IWX_PHY_BAND_5;cmd.ci.channel=ax.bss.channel;
    cmd.ci.width=IWX_PHY_VHT_CHANNEL_MODE20;cmd.ci.ctrl_pos=IWX_PHY_VHT_CTRL_POS_1_BELOW;
    cmd.lmac_id=(ax.bss.channel>14 && firmware_cap(IWX_UCODE_TLV_CAPA_CDB_SUPPORT))?IWX_LMAC_5G_INDEX:IWX_LMAC_24G_INDEX;
    int rc;
    if(firmware_cap(IWX_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS)) {
        /* API-77 advertises the extended channel-info ABI even on AX200,
         * which has no 6 GHz radio. The capability changes the wire layout. */
        struct iwx_phy_context_cmd_uhb ext={0};ext.action=action;ext.lmac_id=cmd.lmac_id;
        ext.ci.band=cmd.ci.band;ext.ci.channel=ax.bss.channel;ext.ci.width=cmd.ci.width;ext.ci.ctrl_pos=cmd.ci.ctrl_pos;
        rc=command(0,IWX_PHY_CONTEXT_CMD,&ext,sizeof ext);
    } else rc=command(0,IWX_PHY_CONTEXT_CMD,&cmd,sizeof cmd);
    if(rc) return -1;
    if(action==IWX_FW_CTXT_ACTION_ADD) ax.phy_active=1;
    if(action!=IWX_FW_CTXT_ACTION_REMOVE) {
        struct iwx_rlc_config_cmd rlc={0};
        rlc.rlc.rx_chain_info=(ax.status.rx_ant<<IWX_PHY_RX_CHAIN_VALID_POS)|
            (1U<<IWX_PHY_RX_CHAIN_CNT_POS)|(1U<<IWX_PHY_RX_CHAIN_MIMO_CNT_POS);
        if(command_version(IWX_DATA_PATH_GROUP,IWX_RLC_CONFIG_CMD,2,&rlc,sizeof rlc)) return -1;
    }
    return 0;
}
static void ack_rates(const struct wifi_bss *b,uint32_t *cck,uint32_t *ofdm) {
    static const uint8_t rates[]={2,4,11,22,12,18,24,36,48,72,96,108};*cck=0;*ofdm=0;
    for(unsigned i=0;i<b->rates_len;i++) if(b->rates[i]&128) {
        for(unsigned j=0;j<sizeof rates;j++) if(rates[j]==(b->rates[i]&127)) {
            if(j<4) *cck|=1U<<j;else *ofdm|=1U<<(j-4);
        }
    }
    if(!*cck && !*ofdm) {if(b->channel<=14) *cck=1;else *ofdm=1;}
}
static int mac_context(unsigned action,int associated) {
    struct iwx_mac_ctx_cmd cmd={0};cmd.action=action;cmd.mac_type=IWX_FW_MAC_TYPE_BSS_STA;
    memcpy(cmd.node_addr,ax.status.mac,6);memcpy(cmd.bssid_addr,ax.bss.ap,6);
    uint32_t cck,ofdm;ack_rates(&ax.bss,&cck,&ofdm);cmd.cck_rates=cck;cmd.ofdm_rates=ofdm;
    cmd.cck_short_preamble=(ax.bss.capabilities&32)?IWX_MAC_FLG_SHORT_PREAMBLE:0;
    cmd.short_slot=(ax.bss.capabilities&1024)?IWX_MAC_FLG_SHORT_SLOT:0;
    cmd.filter_flags=IWX_MAC_FILTER_ACCEPT_GRP|IWX_MAC_FILTER_DIS_DECRYPT|IWX_MAC_FILTER_DIS_GRP_DECRYPT|
        IWX_MAC_FILTER_IN_BEACON; /* keep beacon liveness and all CCMP bytes */
    for(unsigned i=0;i<4;i++) {cmd.ac[i].cw_min=15;cmd.ac[i].cw_max=1023;cmd.ac[i].aifsn=3;cmd.ac[i].fifos_mask=1U<<i;}
    cmd.sta.bi=ax.bss.beacon_interval;cmd.sta.dtim_interval=ax.bss.beacon_interval*ax.bss.dtim_period;
    cmd.sta.listen_interval=10;cmd.sta.is_assoc=associated;cmd.sta.assoc_id=ax.bss.aid;
    if(associated) {
        unsigned off=ax.bss.dtim_count*ax.bss.beacon_interval*1024;
        cmd.sta.dtim_time=ax.beacon_gp2+off;cmd.sta.dtim_tsf=ax.bss.tsf+off;cmd.sta.assoc_beacon_arrive_time=ax.beacon_gp2;
    }
    return command(0,IWX_MAC_CONTEXT_CMD,&cmd,sizeof cmd);
}
static int binding_context(unsigned action) {
    struct iwx_binding_cmd cmd={0};cmd.action=action;
    cmd.macs[1]=cmd.macs[2]=IWX_FW_CTXT_INVALID;
    cmd.lmac_id=(ax.bss.channel>14 && firmware_cap(IWX_UCODE_TLV_CAPA_CDB_SUPPORT))?IWX_LMAC_5G_INDEX:IWX_LMAC_24G_INDEX;
    if(command(0,IWX_BINDING_CONTEXT_CMD,&cmd,sizeof cmd) || response_status()!=0) return -1;
    return 0;
}
static int station_context(int update) {
    struct iwx_add_sta_cmd cmd={0};cmd.add_modify=update;cmd.station_type=IWX_STA_LINK;
    if(!update) memcpy(cmd.addr,ax.bss.ap,6);
    cmd.station_flags_msk=IWX_STA_FLG_FAT_EN_MSK|IWX_STA_FLG_MIMO_EN_MSK;
    if(command(0,IWX_ADD_STA,&cmd,sizeof cmd) || (response_status()&IWX_ADD_STA_STATUS_MASK)!=IWX_ADD_STA_SUCCESS) return -1;
    return 0;
}
static int tx_queue_add(void) {
    struct iwx_scd_queue_cfg_cmd cmd={0};cmd.operation=IWX_SCD_QUEUE_ADD;
    cmd.u.add.sta_mask=1;cmd.u.add.tid=IWX_MGMT_TID;cmd.u.add.cb_size=5;
    cmd.u.add.bc_dram_addr=V2P(&ax.tx_bc);cmd.u.add.tfdq_dram_addr=V2P(ax.tx_desc);
    if(command(IWX_DATA_PATH_GROUP,IWX_SCD_QUEUE_CONFIG_CMD,&cmd,sizeof cmd) || ax.response_len!=sizeof(struct iwx_tx_queue_cfg_rsp)) return -1;
    struct iwx_tx_queue_cfg_rsp rsp;memcpy(&rsp,ax.response,sizeof rsp);
    if(rsp.flags || !rsp.queue_number || rsp.queue_number>255 || rsp.write_pointer) return -1;
    ax.tx_qid=rsp.queue_number;ax.tx_next=0;ax.tx_active=1;return 0;
}
int ax200_prepare_bss(const struct wifi_bss *b) {
    if(!ax_storage || !b || !ax.running || ax.status.state!=AX200_RADIO_READY || ax.status.rfkill ||
       !(channel_flags(b->channel)&IWX_NVM_CHANNEL_VALID) ||
       !(channel_flags(b->channel)&IWX_NVM_CHANNEL_ACTIVE) ||
       (channel_flags(b->channel)&(IWX_NVM_CHANNEL_RADAR|IWX_NVM_CHANNEL_DFS)) ||
       ax.mac_active || scan_abort()) return -1;
    ax.bss=*b;ax.channel=b->channel;ax.beacon_gp2=ax.rx_gp2;
    unsigned phy_version=ax200_fw_command_version(&ax.fw,1,IWX_PHY_CONTEXT_CMD,0);
    if((phy_version!=3 && phy_version!=4) ||
       ax200_fw_command_version(&ax.fw,IWX_DATA_PATH_GROUP,IWX_RLC_CONFIG_CMD,0)!=2) return -1;
    uint32_t cck,ofdm;ack_rates(b,&cck,&ofdm);
    static const uint8_t plcp[]={10,20,55,110,13,15,5,7,9,11,1,3};unsigned rate=0;
    if(cck) {while(!(cck&(1U<<rate))) rate++;}
    else {while(!(ofdm&(1U<<rate))) rate++;rate+=4;}
    unsigned v2=ax200_fw_command_version(&ax.fw,1,IWX_TX_CMD,1)>6;
    ax.rate=(ax.status.tx_ant&1)?IWX_RATE_MCS_ANT_A_MSK:IWX_RATE_MCS_ANT_B_MSK;
    ax.rate|=v2?(rate<4?(IWX_RATE_MCS_CCK_MSK|rate):(IWX_RATE_MCS_LEGACY_OFDM_MSK|(rate-4))):
        (plcp[rate]|(rate<4?IWX_RATE_MCS_CCK_MSK_V1:0));
    if(phy_context(IWX_FW_CTXT_ACTION_ADD)) goto error;
    ax.phy_active=1;
    if(mac_context(IWX_FW_CTXT_ACTION_ADD,0)) goto error;
    ax.mac_active=1;
    if(binding_context(IWX_FW_CTXT_ACTION_ADD)) goto error;
    ax.binding_active=1;
    if(station_context(0)) goto error;
    ax.sta_active=1;
    if(tx_queue_add()) goto error;
    struct iwx_session_prot_cmd protection={.action=IWX_FW_CTXT_ACTION_ADD,.duration_tu=4000};
    if(command(IWX_MAC_CONF_GROUP,IWX_SESSION_PROTECTION_CMD,&protection,sizeof protection)) goto error;
    return 0;
error:
    ax200_leave();return -1;
}
int ax200_associate_bss(const struct wifi_bss *b) {
    if(!ax_storage || !b || !ax.running || !ax.sta_active || memcmp(b->ap,ax.bss.ap,6) || !b->aid || b->aid>2007) return -1;
    ax.bss=*b;
    if(station_context(1) || mac_context(IWX_FW_CTXT_ACTION_MODIFY,1)) return -1;
    struct iwx_mcast_filter_cmd multicast={.filter_own=1,.pass_all=1};memcpy(multicast.bssid,b->ap,6);
    struct iwx_mac_power_cmd power={.keep_alive_seconds=25};
    if(command(0,IWX_MCAST_FILTER_CMD,&multicast,(sizeof multicast+3)&~3U) ||
       command(0,IWX_MAC_PM_POWER_TABLE,&power,sizeof power)) return -1;
    return 0;
}
int ax200_tx(const uint8_t *frame,unsigned n) {
    if(!ax_storage || !frame || n<24 || n>WIFI_MPDU_MAX || !ax.running || !ax.tx_active || ax.tx_pending ||
       ax.status.rfkill || frame[0]==0x88 || (frame[0]&3) || ((frame[0]&0x0c)!=0 && (frame[0]&0x0c)!=8)) return -1;
    unsigned i=ax.tx_next;uint8_t *p=ax.tx[i];memset(p,0,64);
    struct iwx_cmd_header *h=(void *)p;h->code=IWX_TX_CMD;h->idx=i;h->qid=ax.tx_qid;
    struct iwx_tx_cmd_gen2 *cmd=(void *)(p+4);cmd->len=n;cmd->offload_assist=IWX_TX_CMD_OFFLD_MH_SIZE(12);
    cmd->flags=IWX_TX_FLAGS_CMD_RATE|IWX_TX_FLAGS_ENCRYPT_DIS|IWX_TX_FLAGS_HIGH_PRI;cmd->rate_n_flags=ax.rate;
    unsigned header_len=4+sizeof(*cmd)+24;memcpy(p+4+sizeof(*cmd),frame,24);memcpy(p+header_len,frame+24,n-24);
    struct iwx_tfh_tfd *t=&ax.tx_desc[i];memset(t,0,sizeof(*t));t->num_tbs=3;
    t->tbs[0].addr=V2P(p);t->tbs[0].tb_len=20;t->tbs[1].addr=V2P(p+20);t->tbs[1].tb_len=header_len-20;
    t->tbs[2].addr=V2P(p+header_len);t->tbs[2].tb_len=n-24;
    ax.tx_bc.tfd_offset[i]=(n+3)/4;
    if(i<64) ax.tx_bc.tfd_offset[i+256]=(n+3)/4;
    ax.tx_pending=1;ax.tx_completed=0;ax.tx_index=i;ax.tx_next=(i+1)&255;
    publish();wr(IWX_HBUS_TARG_WRPTR,(ax.tx_qid<<16)|ax.tx_next);
    uint64_t start=time_get_us();
    while(ax.running && !ax.tx_completed && time_get_us()-start<1000000) {ax200_poll();delay(20);}
    ax.tx_pending=0;
    if(!ax.running) return -1;
    if(!ax.tx_completed) {stop(0x50002);return -1;}
    return ax.tx_result==IWX_TX_STATUS_SUCCESS || ax.tx_result==IWX_TX_STATUS_DIRECT_DONE?0:-1;
}
void ax200_leave(void) {
    if(!ax_storage || !ax.running) return;
    if(scan_abort()) return;
    if(ax.tx_active) {
        struct iwx_scd_queue_cfg_cmd q={.operation=IWX_SCD_QUEUE_REMOVE};q.u.remove.sta_mask=1;q.u.remove.tid=IWX_MGMT_TID;
        if(command(IWX_DATA_PATH_GROUP,IWX_SCD_QUEUE_CONFIG_CMD,&q,sizeof q)) return;
        ax.tx_active=0;
    }
    if(ax.sta_active) {
        struct iwx_rm_sta_cmd sta={0};
        if(command(0,IWX_REMOVE_STA,&sta,sizeof sta)) return;
        ax.sta_active=0;
    }
    if(ax.binding_active) {if(binding_context(IWX_FW_CTXT_ACTION_REMOVE)) return;ax.binding_active=0;}
    if(ax.mac_active) {if(mac_context(IWX_FW_CTXT_ACTION_REMOVE,0)) return;ax.mac_active=0;}
    if(ax.phy_active) {if(phy_context(IWX_FW_CTXT_ACTION_REMOVE)) return;ax.phy_active=0;}
    ax.frame_head=ax.frame_tail=0;memset(&ax.bss,0,sizeof ax.bss);
}
