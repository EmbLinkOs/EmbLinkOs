#include "block/install.h"
#include "block/block.h"
#include "block/partition.h"
#include "fs/embkfs/embkfs.h"
#include "boot/boot_protocol.h"
#include "crypto/hw_random.h"
#include "include/argv.h"
#include "include/kmalloc.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"

static int rd(void *d,uint64_t lba,uint32_t n,void *b) {return embk_block_read(d,lba,n,b);}
static int wr(void *d,uint64_t lba,uint32_t n,const void *b) {return embk_block_write(d,lba,n,b);}
static int flush(void *d) {return embk_block_flush(d);}
static struct install_io io(struct embk_block_device *d) {
    return (struct install_io){d,d->block_count,rd,d->write?wr:NULL,d->flush?flush:NULL};
}
static uint64_t last_percent;
static void progress(uint64_t at,uint64_t end) {
    uint64_t pct=at*100/end;
    if(pct/10!=last_percent/10) {last_percent=pct;kprintf("install: copied and verified %llu%%\n",pct);}
}
int install_command(const char *line) {
    if(strncmp(line,"install",7) || (line[7] && line[7]!=' ')) return 0;
    char buf[128],*args[6];int n=console_argv(line,buf,sizeof buf,args,5);
    struct embkfs_volume *root=embkfs_live_volume();
    struct embk_block_device *source=root?embk_partition_parent(root->dev):NULL;
    if(n==2 && !strcmp(args[1],"list")) {
        for(unsigned i=0;i<embk_block_count();i++) {
            struct embk_block_device *d=embk_block_get(i);
            if(embk_partition_parent(d)!=d) continue;
            kprintf("install: %s: %llu MiB, %u-byte sectors%s%s\n",d->name,
                d->block_count*d->block_size/1048576,d->block_size,
                d==source?" [running source disk]":"",d->write?"":" [read-only]");
            const struct embk_block_identity *id=embk_block_identity(d);
            if(id && id->model[0])kprintf("  %s: %s; serial %s\n",id->transport,id->model,id->serial[0]?id->serial:"unavailable");
        }
        return 1;
    }
    if(n<3 || n>4 || (strcmp(args[1],"plan") && strcmp(args[1],"erase"))) {
        kprintf("install list\ninstall plan <disk>\ninstall erase <disk> ERASE-<disk>-<plan fingerprint>\n");return 1;
    }
    if(!boot_recovery_mode() || !root || !root->read_only) {
        kprintf("install: choose Recovery in EmbBoot first; source must be read-only\n");return 1;
    }
    struct embk_block_device *target=embk_block_get_by_name(args[2]);
    if(!target || !source || target==source || embk_partition_parent(target)!=target ||
       source->block_size!=512 || target->block_size!=512) {
        kprintf("install: select a different whole disk with 512-byte sectors\n");return 1;
    }
    for(unsigned i=0;i<embkfs_volume_count();i++)
        if(embk_partition_parent(embkfs_volume_at(i)->dev)==target) {
            kprintf("install: target has a mounted filesystem; refused\n");return 1;
        }
    struct install_io s=io(source),d=io(target);
    struct install_plan *p=kmalloc(sizeof *p);
    if(!p) {kprintf("install: out of memory\n");return 1;}
    int rc=install_prepare(&s,&d,p);
    if(rc) {kprintf("install: incompatible source/target GPT or I/O error (%d)\n",rc);kfree(p);return 1;}
    char confirm[80];
    snprintf(confirm,sizeof confirm,"ERASE-%s-%08x",target->name,p->fingerprint);
    kprintf("install: source %s -> target %s (%llu MiB)\n",source->name,target->name,target->block_count/2048);
    kprintf("install: target's existing partitions and files will be overwritten\n");
    kprintf("install: EFI %llu MiB + EMBKFS %llu MiB; root grows to use target capacity\n",p->esp_sectors/2048,p->target_root_sectors/2048);
    if(strcmp(args[1],"erase") || n!=4 || strcmp(args[3],confirm)) {
        kprintf("install: reviewed plan: install erase %s %s\n",target->name,confirm);
        kfree(p);return 1;
    }
    uint8_t ids[52];
    void *buffer=kmalloc(65536),*verify=kmalloc(65536);
    if(!buffer || !verify || crypto_hw_random(ids,sizeof ids)) rc=-EMBK_ENOMEM;
    else {
        /* The recovery session has no userspace writers. Hold the filesystem
         * lock as well, so diagnostic commands cannot race this snapshot. */
        embkfs_lock();
        rc=embkfs_check(root);
        if(!rc) {last_percent=0;rc=install_copy(&s,&d,p,ids,buffer,verify,65536,progress);}
        embkfs_unlock();
    }
    memset(ids,0,sizeof ids);kfree(buffer);kfree(verify);kfree(p);
    if(rc) kprintf("install: FAILED (%d). Do not boot the target; retry from recovery media.\n",rc);
    else kprintf("install: verified installation complete. Shut down, remove source media, boot target with UEFI.\n");
    return 1;
}
