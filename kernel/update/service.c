#include "service.h"
#include "protocol.h"
#include "boot/boot_protocol.h"
#include "block/partition.h"
#include "fs/embkfs/embkfs.h"
#include "fs/fat32.h"
#include "crypto/sha256.h"
#include "process/process.h"
#include "process/capabilities.h"
#include "process/ksync.h"
#include "drivers/timer/timer.h"
#include "include/errno.h"
#include "include/kmalloc.h"
#include "include/kstring.h"
#include "include/kprintf.h"

static struct {
    struct mutex lock;int ready,record,worker,writing;
    struct update_state state;struct update_slot candidate;
    struct embk_block_device *esp,*root[2];struct fat32_volume fat;
    uint64_t *efi_map;uint32_t efi_sectors;
    unsigned target,verify_mode;uint64_t root_done,efi_done,healthy_since,verify_done;
    struct sha256_ctx verify_hash;
    struct embk_update_info info;
} u;
static uint8_t page[4096] __attribute__((aligned(4096)));
static int admin(void) {
    return current_thread && ((current_process->session_admin &&
        (process_current_caps()&EMBK_CAP_BIT(EMBK_CAP_POWER))) ||
        (!current_process->session_id && (process_current_caps()&EMBK_CAP_BIT(EMBK_CAP_SESSION))));
}
static void message(unsigned phase,const char *text,int error) {
    u.info.phase=phase;u.info.error=error;strncpy(u.info.message,text,sizeof u.info.message-1);
    u.info.message[sizeof u.info.message-1]=0;
}
static int persist(struct update_state *next) {
    uint8_t expected[512];int rc=update_state_encode(next,expected);if(rc)return -EMBK_EINVAL;
    memset(page,0,sizeof page);memcpy(page,expected,512);
    uint64_t off=u.record?UPDATE_CONTROL0_OFFSET:UPDATE_CONTROL1_OFFSET;
    rc=embk_block_write(u.esp,off/512,8,page);
    if(!rc)rc=embk_block_flush(u.esp);
    if(!rc)rc=embk_block_read(u.esp,off/512,8,page);
    if(!rc && memcmp(page,expected,512))rc=-EMBK_EIO;
    if(!rc){u.state=*next;u.record=1-u.record;}
    else {u.ready=0;message(EMBK_UPDATE_ERROR,"Boot journal I/O failed. Restart before trying another update.",rc);}
    return rc;
}
int update_service_init(void) {
    memset(&u,0,sizeof u);mutex_init(&u.lock);u.info.size=sizeof u.info;
    message(EMBK_UPDATE_UNAVAILABLE,"This installation uses the legacy disk layout. Managed installation is required for system upgrades.",0);
    const struct boot_protocol *bp=boot_protocol_get();
    if(!bp || bp->size<BOOT_PROTOCOL_SIZE_V4 || !(bp->update_flags&1) || boot_recovery_mode())return 0;
    struct embk_block_device *disk=embk_partition_boot_disk();
    for(uint32_t i=0;i<embk_block_count();i++) {
        struct embk_block_device *dev=embk_block_get(i);
        if(!dev || dev==disk || embk_partition_parent(dev)!=disk || dev->block_size!=512)continue;
        if(!embk_block_read(dev,0,1,page) && !memcmp(page+3,"EMBUPD1 ",8)) {
            if(u.esp)return -EMBK_EINVAL;u.esp=dev;
        }
    }
    if(!u.esp || !disk->flush)return -EMBK_ENOTSUP;
    uint8_t records[2][512];
    if(embk_block_read(u.esp,UPDATE_CONTROL0_OFFSET/512,1,records[0]) ||
       embk_block_read(u.esp,UPDATE_CONTROL1_OFFSET/512,1,records[1]))return -EMBK_EIO;
    u.record=update_state_choose(records[0],records[1],&u.state);if(u.record<0)return -EMBK_EIO;
    uint8_t guid[16];if(embk_partition_guid(u.esp,guid) || memcmp(guid,u.state.esp_guid,16) ||
       memcmp(bp->update_data_guid,u.state.data_guid,16) || bp->update_slot>1 ||
       memcmp(bp->update_root_guid,u.state.slot[bp->update_slot].root_guid,16) ||
       bp->update_revision!=u.state.revision)return -EMBK_EINVAL;
    for(unsigned i=0;i<2;i++) {
        u.root[i]=embk_partition_by_guid(u.state.slot[i].root_guid,disk);
        if(!u.root[i] || u.root[i]->block_size!=512)return -EMBK_ENODEV;
    }
    if(fat32_mount(u.esp,&u.fat))return -EMBK_EIO;
    u.info.flags=EMBK_UPDATE_MANAGED;
    u.info.release=u.state.slot[bp->update_slot].release;
    memcpy(u.info.current,u.state.slot[bp->update_slot].version,32);
    if(u.state.pending!=UPDATE_NO_SLOT && bp->update_slot==u.state.pending)u.info.flags|=EMBK_UPDATE_TRIAL;
    if(u.state.flags&UPDATE_REVERTED)u.info.flags|=EMBK_UPDATE_REVERTED;
    u.ready=1;message(EMBK_UPDATE_IDLE,"Update service is starting.",0);
    if(u.state.pending!=UPDATE_NO_SLOT && bp->update_slot!=u.state.pending)message(EMBK_UPDATE_RESTART,"A system update is ready. Restart to try it.",0);
    kprintf("update: managed release %llu %s; journal revision %llu\n",(unsigned long long)u.info.release,u.info.current,(unsigned long long)u.state.revision);
    return 0;
}
static void staging_clear(void) {kfree(u.efi_map);u.efi_map=NULL;u.efi_sectors=0;u.writing=0;u.verify_mode=0;}
static int map_slot(unsigned slot,uint64_t **map,uint32_t *n) {
    return fat32_fixed_map(&u.fat,slot?"/EFI/EMBLINK/SLOT1.EFI":"/EFI/EMBLINK/SLOT0.EFI",UPDATE_EFI_CAPACITY,map,n);
}
static int begin(const struct embk_update_image *image) {
    if(u.writing || u.state.pending!=UPDATE_NO_SLOT)return -EMBK_EBUSY;
    u.target=1-u.state.active;
    struct update_slot candidate={0};memcpy(candidate.root_guid,u.state.slot[u.target].root_guid,16);
    candidate.release=image->release;candidate.root_bytes=image->root_bytes;candidate.efi_bytes=image->efi_bytes;
    memcpy(candidate.version,image->version,32);memcpy(candidate.root_sha256,image->root_sha256,32);memcpy(candidate.efi_sha256,image->efi_sha256,32);
    struct update_state test=u.state;
    if(update_state_stage(&test,u.target,&candidate))return -EMBK_EINVAL;
    if(candidate.root_bytes/512>u.root[u.target]->block_count)return -EMBK_ENOSPC;
    /* Never overwrite a mounted volume, including a non-root mount. */
    for(unsigned i=0;i<embkfs_volume_count();i++)if(embkfs_volume_at(i)->dev==u.root[u.target])return -EMBK_EBUSY;
    int rc=map_slot(u.target,&u.efi_map,&u.efi_sectors);if(rc)return rc;
    uint8_t *owned=kcalloc((u.esp->block_count+7)/8,1);uint64_t *other=NULL;uint32_t n=0;
    if(!owned){staging_clear();return -EMBK_ENOMEM;}
    for(unsigned i=0;i<u.efi_sectors;i++)owned[u.efi_map[i]/8]|=1u<<(u.efi_map[i]%8);
    for(unsigned which=0;which<2 && !rc;which++) {
        rc=which?fat32_fixed_map(&u.fat,"/EFI/BOOT/BOOTX64.EFI",0,&other,&n):map_slot(u.state.active,&other,&n);
        if(!rc)for(unsigned i=0;i<n;i++)if(owned[other[i]/8]&(1u<<(other[i]%8))){rc=-EMBK_EIO;break;}
        kfree(other);other=NULL;
    }
    kfree(owned);if(rc){staging_clear();return rc;}
    struct update_state next=u.state;
    if(update_state_begin_write(&next,u.target)){staging_clear();return -EMBK_EINVAL;}
    rc=persist(&next);if(rc){staging_clear();return rc;}
    u.candidate=candidate;u.root_done=u.efi_done=0;u.writing=1;
    u.info.done=0;u.info.total=candidate.root_bytes+candidate.efi_bytes;
    message(EMBK_UPDATE_WRITING,"Installing to the inactive system slot. You can keep working.",0);return 0;
}
static int write_image(int efi,const uint8_t *data,uint64_t len) {
    if(!u.writing || !len || len>EMBK_UPDATE_CHUNK)return -EMBK_EINVAL;
    uint64_t *done=efi?&u.efi_done:&u.root_done,total=efi?u.candidate.efi_bytes:u.candidate.root_bytes;
    if(*done>total || len>total-*done || (*done&511) || ((len&511) && (!efi || len!=total-*done)))return -EMBK_EINVAL;
    int rc=0;
    if(!efi)rc=embk_block_write(u.root[u.target],*done/512,len/512,data);
    else for(uint64_t i=0;i<len && !rc;i+=512) {
        uint64_t take=len-i;if(take>512)take=512;
        memset(page,0,512);memcpy(page,data+i,take);
        rc=embk_block_write(u.esp,u.efi_map[(*done+i)/512],1,page);
    }
    if(!rc){*done+=len;u.info.done=u.root_done+u.efi_done;u.info.progress=(uint32_t)(u.info.done*100/u.info.total);}return rc;
}
/* Verification advances in bounded chunks. Settings can poll between calls
 * instead of blocking behind a full-image readback held under the mutex. */
static int commit_image(void) {
    if(!u.writing || u.root_done!=u.candidate.root_bytes || u.efi_done!=u.candidate.efi_bytes)return -EMBK_EINVAL;
    if(!u.verify_mode) {
        int rc=embk_block_flush(u.root[u.target]);if(!rc)rc=embk_block_flush(u.esp);if(rc)return rc;
        u.verify_mode=1;u.verify_done=0;sha256_init(&u.verify_hash);u.info.done=0;u.info.progress=0;
        message(EMBK_UPDATE_VERIFYING,"Reading back and verifying the installed system.",0);
    }
    int efi=u.verify_mode==2;uint64_t total=efi?u.candidate.efi_bytes:u.candidate.root_bytes;
    uint64_t end=u.verify_done+EMBK_UPDATE_CHUNK;if(end>total)end=total;
    while(u.verify_done<end) {
        uint64_t off=u.verify_done;size_t n=end-off;if(n>4096)n=4096;int rc=0;
        if(!efi)rc=embk_block_read(u.root[u.target],off/512,n/512,page);
        else for(size_t i=0;i<n && !rc;i+=512)rc=embk_block_read(u.esp,u.efi_map[(off+i)/512],1,page+i);
        if(rc)return rc;sha256_update(&u.verify_hash,page,n);u.verify_done+=n;
    }
    u.info.done=(efi?u.candidate.root_bytes:0)+u.verify_done;
    u.info.progress=(uint32_t)(u.info.done*100/u.info.total);
    if(u.verify_done<total)return 1;
    uint8_t digest[32];sha256_final(&u.verify_hash,digest);
    if(memcmp(digest,efi?u.candidate.efi_sha256:u.candidate.root_sha256,32))return -EMBK_EIO;
    if(!efi){u.verify_mode=2;u.verify_done=0;sha256_init(&u.verify_hash);return 1;}
    struct update_state next=u.state;
    int rc=update_state_stage(&next,u.target,&u.candidate)?-EMBK_EINVAL:persist(&next);
    if(!rc){staging_clear();message(EMBK_UPDATE_RESTART,"Ready to restart. Your files and settings stay in place.",0);kprintf("update: verified release %llu staged in slot %c\n",(unsigned long long)u.candidate.release,'A'+u.target);}return rc;
}
/* Caller passes kernel bounce buffers, never userspace addresses. Internal
 * operations belong to one init-started worker PID, not an ambient GUI cap. */
int update_service_control(unsigned op,void *data,uint64_t len) {
    mutex_lock(&u.lock);int rc=0;
    if(op==EMBK_UPDATE_STATUS) {
        if(len!=sizeof u.info)rc=-EMBK_EINVAL;
        else {struct embk_update_info info=u.info;info.flags&=~(EMBK_UPDATE_ADMIN|EMBK_UPDATE_PREVIOUS);
            if(admin())info.flags|=EMBK_UPDATE_ADMIN;
            if(u.state.previous!=UPDATE_NO_SLOT && u.ready)info.flags|=EMBK_UPDATE_PREVIOUS;
            info.flags&=~EMBK_UPDATE_REVERTED;if(u.ready && (u.state.flags&UPDATE_REVERTED))info.flags|=EMBK_UPDATE_REVERTED;
            memcpy(data,&info,sizeof info);}
        goto out;
    }
    if(!u.ready){rc=-EMBK_ENOTSUP;goto out;}
    if(op==EMBK_UPDATE_WORKER) {
        if(!current_thread || current_process->session_id || !(process_current_caps()&EMBK_CAP_BIT(EMBK_CAP_SESSION))){rc=-EMBK_EPERM;goto out;}
        if(u.worker && process_alive(u.worker)){rc=-EMBK_EBUSY;goto out;}
        if(u.writing) {
            struct update_state next=u.state;
            rc=update_state_reject(&next,UPDATE_WRITE_FAILED)?-EMBK_EIO:persist(&next);
            staging_clear();if(rc)goto out;
        }
        u.info.job=0;u.worker=current_process->pid;goto out;
    }
    if(op==EMBK_UPDATE_CHECK || op==EMBK_UPDATE_INSTALL || op==EMBK_UPDATE_ROLLBACK || op==EMBK_UPDATE_APPLICATIONS) {
        if(!admin()){rc=-EMBK_EPERM;goto out;}
        if(u.writing || u.info.job || (u.info.flags&EMBK_UPDATE_TRIAL)){rc=-EMBK_EBUSY;goto out;}
        if(op==EMBK_UPDATE_ROLLBACK) {
            struct update_state next=u.state;
            rc=update_state_rollback(&next)?-EMBK_EINVAL:persist(&next);
            if(!rc)message(EMBK_UPDATE_RESTART,"Previous system selected. Restart to try it. Your files remain unchanged.",0);goto out;
        }
        if(u.state.pending!=UPDATE_NO_SLOT){rc=-EMBK_EBUSY;goto out;}
        if(!u.worker || !process_alive(u.worker)){rc=-EMBK_ENODEV;goto out;}
        u.info.job=op;message(op==EMBK_UPDATE_CHECK?EMBK_UPDATE_CHECKING:EMBK_UPDATE_DOWNLOADING,"Contacting the configured update channel.",0);goto out;
    }
    if(!current_thread || current_process->pid!=(uint32_t)u.worker){rc=-EMBK_EPERM;goto out;}
    switch(op) {
    case EMBK_UPDATE_JOB:rc=(int)u.info.job;break;
    case EMBK_UPDATE_REPORT: {
        if(len!=sizeof u.info){rc=-EMBK_EINVAL;break;}
        struct embk_update_info *info=data;
        if(info->phase>EMBK_UPDATE_ERROR || info->progress>100){rc=-EMBK_EINVAL;break;}
        if(!u.writing && u.state.pending==UPDATE_NO_SLOT)message(info->phase,info->message,info->error);
        u.info.flags=(u.info.flags&~EMBK_UPDATE_APPS_CHANNEL)|(info->flags&EMBK_UPDATE_APPS_CHANNEL);
        u.info.candidate_release=info->candidate_release;memcpy(u.info.candidate,info->candidate,32);u.info.candidate[31]=0;
        memcpy(u.info.notes,info->notes,256);u.info.notes[255]=0;
        if(!info->job)u.info.job=0;break;
    }
    case EMBK_UPDATE_BEGIN:rc=len==sizeof(struct embk_update_image)?begin(data):-EMBK_EINVAL;break;
    case EMBK_UPDATE_WRITE_ROOT:case EMBK_UPDATE_WRITE_EFI:rc=write_image(op==EMBK_UPDATE_WRITE_EFI,data,len);break;
    case EMBK_UPDATE_COMMIT:rc=commit_image();break;
    case EMBK_UPDATE_ABORT: {
        if(u.writing){struct update_state next=u.state;rc=update_state_reject(&next,UPDATE_WRITE_FAILED)?-EMBK_EIO:persist(&next);staging_clear();}
        u.info.job=0;if(!rc)message(EMBK_UPDATE_ERROR,"Installation stopped. The current system remains selected.",-EMBK_EIO);break;
    }
    default:rc=-EMBK_EINVAL;
    }
out:mutex_unlock(&u.lock);return rc;
}
void update_service_health(int ready) {
    if(!u.ready || !(u.info.flags&EMBK_UPDATE_TRIAL))return;
    mutex_lock(&u.lock);
    uint64_t now=time_get_ns()/1000000;
    if(!ready || !u.worker || !process_alive(u.worker))u.healthy_since=0;
    else if(!u.healthy_since)u.healthy_since=now;
    else if(now-u.healthy_since>=5000) {
        struct update_state next=u.state;unsigned running=boot_protocol_get()->update_slot;
        int rc=update_state_confirm(&next,running)?-EMBK_EINVAL:persist(&next);
        if(!rc){u.info.flags&=~EMBK_UPDATE_TRIAL;message(EMBK_UPDATE_IDLE,"System upgrade completed. Previous system retained for rollback.",0);kprintf("update: healthy desktop confirmed release %llu in slot %c\n",(unsigned long long)u.info.release,'A'+running);}
        else message(EMBK_UPDATE_ERROR,"Could not confirm this boot. The next restart will restore the previous system.",rc);
    }
    mutex_unlock(&u.lock);
}
