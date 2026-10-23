/* Read-only integrity scan. No repair, allocation bitmap rebuild or commits. */
#include "fs/embkfs/embkfs.h"
#include "block/block.h"
#include "include/kmalloc.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#include "include/errno.h"

struct check {
    struct embkfs_volume *vol;
    uint8_t *seen,*data;
    uint64_t nodes,files,bytes;
};
static int cmp(const struct embk_key *a,const struct embk_key *b) {
    if(a->object_id!=b->object_id) return a->object_id<b->object_id?-1:1;
    if(a->type!=b->type) return a->type<b->type?-1:1;
    return a->offset==b->offset?0:a->offset<b->offset?-1:1;
}
static int walk(struct check *c,const struct embk_block_ptr *ptr,int expected,
                const struct embk_key *lower,const struct embk_key *upper,unsigned depth) {
    struct embkfs_volume *v=c->vol;
    if(depth>16 || ptr->block>=v->total_blocks ||
       (c->seen[ptr->block/8]&(1u<<(ptr->block%8)))) return -EMBK_EINVAL;
    c->seen[ptr->block/8]|=1u<<(ptr->block%8);
    uint8_t *node=kmalloc(v->block_size);
    if(!node) return -EMBK_ENOMEM;
    int rc=embkfs_read_node(v,ptr,node,v->block_size);
    if(rc) {kfree(node);return rc;}
    const struct embk_node_header *h=(const void *)node;
    unsigned slot_size=h->level?sizeof(struct embk_internal_slot):sizeof(struct embk_item_header);
    if(h->level>16 || (expected>=0 && h->level!=expected) ||
       h->nritems>(v->block_size-sizeof *h)/slot_size || (!h->nritems && (lower || h->level))) {
        kfree(node);return -EMBK_EINVAL;
    }
    c->nodes++;
    struct embk_key prev={0};
    for(unsigned i=0;i<h->nritems && !rc;i++) {
        const uint8_t *slot=node+sizeof *h+(size_t)i*slot_size;
        struct embk_key key;memcpy(&key,slot,sizeof key);
        if((i && cmp(&prev,&key)>=0) || (!i && lower && cmp(lower,&key)) ||
           (upper && cmp(&key,upper)>=0)) {rc=-EMBK_EINVAL;break;}
        prev=key;
        if(h->level) {
            struct embk_internal_slot s;memcpy(&s,slot,sizeof s);
            struct embk_key next;
            const struct embk_key *limit=upper;
            if(i+1<h->nritems) {memcpy(&next,slot+slot_size,sizeof next);limit=&next;}
            rc=walk(c,&s.ptr,h->level-1,&key,limit,depth+1);
            continue;
        }
        struct embk_item_header item;memcpy(&item,slot,sizeof item);
        uint64_t table_end=sizeof *h+(uint64_t)h->nritems*slot_size;
        if(item.offset<table_end || item.offset>v->block_size || item.size>v->block_size-item.offset) {
            rc=-EMBK_EINVAL;break;
        }
        for(unsigned j=0;j<i;j++) {
            struct embk_item_header old;memcpy(&old,node+sizeof *h+(size_t)j*slot_size,sizeof old);
            if(item.offset<(uint64_t)old.offset+old.size && old.offset<(uint64_t)item.offset+item.size)
                rc=-EMBK_EINVAL;
        }
        if(rc || key.type!=EMBK_TYPE_INODE) continue;
        if(item.size!=sizeof(struct embk_inode_item)) {rc=-EMBK_EINVAL;break;}
        struct embk_inode_item ino;memcpy(&ino,node+item.offset,sizeof ino);
        if((ino.mode&0170000)!=0100000) continue;
        c->files++;
        for(uint64_t off=0;off<ino.size;) {
            uint64_t count=ino.size-off<65536?ino.size-off:65536,got=0;
            rc=embkfs_read_object_at(v,key.object_id,off,c->data,count,&got);
            if(rc || got!=count) {if(!rc) rc=-EMBK_EIO;break;}
            off+=got;c->bytes+=got;
        }
    }
    kfree(node);return rc;
}
int embkfs_check(struct embkfs_volume *v) {
    if(!v || !v->total_blocks || v->total_blocks>SIZE_MAX-7) return -EMBK_EINVAL;
    struct check c={.vol=v};
    c.seen=kmalloc((v->total_blocks+7)/8);c.data=kmalloc(65536);
    if(!c.seen || !c.data) {kfree(c.seen);kfree(c.data);return -EMBK_ENOMEM;}
    memset(c.seen,0,(v->total_blocks+7)/8);
    embkfs_lock();
    /* Check durable media again, including files warmed by login/font loading
     * or a previous scan. Cached data cannot establish on-disk integrity. */
    for(unsigned i=0;i<EMBKFS_RCACHE_SLOTS;i++)v->rcache[i].oid=0;
    v->ecache_oid=0;v->wcache_oid=0;
    for(unsigned i=0;i<sizeof v->icache/sizeof v->icache[0];i++)v->icache[i].valid=false;
    struct embk_inode_item root;
    int rc=embkfs_stat_object(v,EMBKFS_ROOT_OBJECT_ID,&root);
    if(!rc && (root.mode&0170000)!=0040000)rc=-EMBK_EINVAL;
    if(!rc)rc=walk(&c,&v->root,-1,NULL,NULL,0);
    embkfs_unlock();
    kprintf("recovery: %s: integrity %s (%llu nodes, %llu files, %llu bytes; error %d)\n",
        v->dev->name,rc?"FAILED":"OK",c.nodes,c.files,c.bytes,rc);
    kfree(c.seen);kfree(c.data);return rc;
}
