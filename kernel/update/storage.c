#include "storage.h"
#include "boot/boot_protocol.h"
#include "block/partition.h"
#include "fs/embkfs/embkfs.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "include/errno.h"
#include "include/kstring.h"
#include "include/kprintf.h"

/* Only the merged root is virtual. Every selected app is an ordinary vnode
 * from its physical version directory, so open handles/narrow namespace binds
 * retain the old bundle while future launches can resolve a new CURRENT. */
static struct {struct vnode lower,upper,store;bool have_upper,have_store;} apps;
static bool app_name(const char *p,size_t n) {
    if(!n || n>63 || p[0]=='.')return false;
    for(size_t i=0;i<n;i++)if(!((p[i]>='a' && p[i]<='z') || (p[i]>='A' && p[i]<='Z') ||
        (p[i]>='0' && p[i]<='9') || p[i]=='-' || p[i]=='_' || p[i]=='.'))return false;
    return true;
}
static int lookup(struct vnode *dir,const char *name,size_t n,struct vnode *out) {
    if(!dir->mnt || !dir->mnt->ops || !dir->mnt->ops->lookup)return -EMBK_ENOENT;
    return dir->mnt->ops->lookup(dir,name,n,out);
}
/* 0 absent/unpublished, 1 active, negative corruption. Pointer format is a
 * canonical lowercase SHA-256 id followed by newline; optional previous id
 * follows in a second line. The publisher atomically replaces this file. */
static int modern(const char *name,size_t n,struct vnode *out) {
    if(!apps.have_store || !app_name(name,n))return 0;
    struct vnode package,current,versions;
    int rc=lookup(&apps.store,name,n,&package);if(rc==-EMBK_ENOENT)return 0;if(rc)return rc;
    if(package.type!=VFS_DT_DIR)return -EMBK_EIO;
    rc=lookup(&package,"CURRENT",7,&current);if(rc==-EMBK_ENOENT)return 0;if(rc)return rc;
    if(current.type!=VFS_DT_REG || !current.mnt->ops->read || !current.mnt->ops->stat)return -EMBK_EIO;
    struct vfs_stat st;if(current.mnt->ops->stat(&current,&st) || (st.size!=65 && st.size!=130))return -EMBK_EIO;
    char pointer[130];size_t got=0;
    if(current.mnt->ops->read(&current,0,pointer,(size_t)st.size,&got) || got!=st.size)return -EMBK_EIO;
    for(size_t i=0;i<got;i++) {
        if(i==64 || i==129){if(pointer[i]!='\n')return -EMBK_EIO;}
        else if(!((pointer[i]>='0' && pointer[i]<='9') || (pointer[i]>='a' && pointer[i]<='f')))return -EMBK_EIO;
    }
    if(lookup(&package,"versions",8,&versions) || versions.type!=VFS_DT_DIR ||
       lookup(&versions,pointer,64,out) || out->type!=VFS_DT_DIR)return -EMBK_EIO;
    return 1;
}
static int app_lookup(struct vnode *dir,const char *name,size_t n,struct vnode *out) {
    (void)dir;int rc=modern(name,n,out);if(rc)return rc==1?0:rc;
    if(apps.have_upper){rc=lookup(&apps.upper,name,n,out);if(rc!=-EMBK_ENOENT)return rc;}
    return lookup(&apps.lower,name,n,out);
}
struct listing {vfs_readdir_cb callback;void *context;unsigned layer;};
static int emit(const char *name,uint8_t n,uint8_t type,uint64_t ino,void *data) {
    struct listing *l=data;struct vnode selected;
    if(!app_name(name,n))return 0;
    int chosen=modern(name,n,&selected);
    if(l->layer==0){if(chosen!=1)return 0;type=VFS_DT_DIR;ino=selected.ino;}
    else {
        if(chosen)return 0;
        if(l->layer==2 && apps.have_upper && !lookup(&apps.upper,name,n,&selected))return 0;
    }
    return l->callback(name,n,type,ino,l->context);
}
static int app_readdir(struct vnode *dir,vfs_readdir_cb cb,void *ctx) {
    (void)dir;struct listing l={cb,ctx,0};int rc=0;
    if(apps.have_store)rc=apps.store.mnt->ops->readdir(&apps.store,emit,&l);
    if(!rc && apps.have_upper){l.layer=1;rc=apps.upper.mnt->ops->readdir(&apps.upper,emit,&l);}
    if(!rc){l.layer=2;rc=apps.lower.mnt->ops->readdir(&apps.lower,emit,&l);}return rc;
}
static struct vnode *writable_apps(void) {return apps.have_upper?&apps.upper:&apps.lower;}
static int app_stat(struct vnode *vn,struct vfs_stat *out) {
    (void)vn;struct vnode *root=writable_apps();return root->mnt->ops->stat(root,out);
}
static int app_create(struct vnode *dir,const char *name,size_t n,uint32_t mode,struct vnode *out) {
    struct vnode exists;if(!app_lookup(dir,name,n,&exists))return -EMBK_EEXIST;
    struct vnode *root=writable_apps();if(!root->mnt->ops->create)return -EMBK_ENOSYS;
    return root->mnt->ops->create(root,name,n,mode,out);
}
static int app_mkdir(struct vnode *dir,const char *name,size_t n,struct vnode *out) {
    struct vnode exists;if(!app_lookup(dir,name,n,&exists))return -EMBK_EEXIST;
    struct vnode *root=writable_apps();if(!root->mnt->ops->mkdir)return -EMBK_ENOSYS;
    return root->mnt->ops->mkdir(root,name,n,out);
}
static int app_rmdir(struct vnode *dir,const char *name,size_t n) {
    (void)dir;struct vnode *root=writable_apps();
    if(!root->mnt->ops->rmdir)return -EMBK_ENOSYS;
    return root->mnt->ops->rmdir(root,name,n);
}
static int app_sync(struct vnode *vn) {
    (void)vn;struct vnode *root=writable_apps();return root->mnt->ops->sync?root->mnt->ops->sync(root):0;
}
static int app_vget(struct vfs_mount *mnt,uint64_t ino,uint8_t type,struct vnode *out) {
    if(ino!=1 || type!=VFS_DT_DIR)return -EMBK_ENOENT;
    *out=mnt->root;return 0;
}
static const struct vfs_ops app_ops={.lookup=app_lookup,.readdir=app_readdir,.create=app_create,
    .mkdir=app_mkdir,.rmdir=app_rmdir,.stat=app_stat,.sync=app_sync,.vget=app_vget};

int update_storage_mount(void) {
    memset(&apps,0,sizeof apps);
    int rc=vfs_resolve("/data/apps",&apps.lower);if(rc)return rc;
    const struct boot_protocol *bp=boot_protocol_get();
    bool managed=bp && bp->size>=BOOT_PROTOCOL_SIZE_V4 && (bp->update_flags&1);
    if(managed) {
        if(boot_recovery_mode())return 0;
        struct embk_block_device *device=embk_partition_by_guid(bp->update_data_guid,embk_partition_boot_disk());
        struct embkfs_volume *volume=NULL;
        for(unsigned i=1;i<embkfs_volume_count();i++)if(embkfs_volume_at(i)->dev==device)volume=embkfs_volume_at(i);
        if(!volume || volume->read_only)return -EMBK_ENODEV;
        char path[64]="/";strcat(path,device->name);size_t base=strlen(path);
        const char *names[]={"/data","/home","/etc"};
        for(unsigned i=0;i<3;i++) {
            path[base]=0;strcat(path,names[i]);struct vnode root;
            if((rc=vfs_resolve(path,&root)) || root.type!=VFS_DT_DIR)return rc?rc:-EMBK_ENOTDIR;
            rc=vfs_mount(names[i],root.mnt->ops,root.mnt->fs_data,root.ino);if(rc)return rc;
        }
        rc=vfs_resolve("/data/apps",&apps.upper);if(rc || apps.upper.type!=VFS_DT_DIR)return rc?rc:-EMBK_ENOTDIR;
        apps.have_upper=true;
        kprintf("update: shared personal data mounted from %s; OS slot remains read-only\n",device->name);
    }
    (void)vfs_mkdir_path("/data/pkg");(void)vfs_mkdir_path("/data/pkg/apps");
    apps.have_store=vfs_resolve("/data/pkg/apps",&apps.store)==0 && apps.store.type==VFS_DT_DIR;
    return vfs_mount("/data/apps",&app_ops,&apps,1);
}
