#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fs/embkfs/check.c"
static uint8_t root_node[4096],leaf[4096];
static int data_error,root_error,depth;
static unsigned data_reads;
void *kmalloc(size_t n){return malloc(n);}
void kfree(void *p){free(p);}
void embkfs_lock(void){depth++;}
void embkfs_unlock(void){assert(depth);depth--;}
void kprintf(const char *f,...){(void)f;}
int embkfs_read_node(struct embkfs_volume *v,const struct embk_block_ptr *p,uint8_t *out,size_t n){assert(v&&n==4096);if(p->block!=17&&p->block!=18)return -EMBK_EIO;memcpy(out,p->block==17?root_node:leaf,n);return 0;}
int embkfs_stat_object(struct embkfs_volume *v,uint64_t oid,struct embk_inode_item *out){assert(oid==1 && !v->rcache[0].oid && !v->ecache_oid && !v->wcache_oid && !v->icache_valid);memset(out,0,sizeof *out);out->mode=0040700;return root_error;}
int embkfs_read_object_at(struct embkfs_volume *v,uint64_t oid,uint64_t off,uint8_t *buf,uint64_t len,uint64_t *got){assert(depth && !v->rcache[0].oid && oid==2 && !off && len==4096);memset(buf,0,len);*got=len;data_reads++;return data_error;}
static void fixture(struct embkfs_volume *v){
    memset(root_node,0,sizeof root_node);memset(leaf,0,sizeof leaf);
    struct embk_node_header *h=(void *)root_node;h->level=1;h->nritems=1;
    struct embk_internal_slot *s=(void *)(root_node+sizeof *h);s->key=(struct embk_key){2,EMBK_TYPE_INODE,0};s->ptr.block=18;
    h=(void *)leaf;h->level=0;h->nritems=1;
    struct embk_item_header *i=(void *)(leaf+sizeof *h);i->key=s->key;i->offset=3968;i->size=128;
    struct embk_inode_item *ino=(void *)(leaf+3968);ino->mode=0100600;ino->size=4096;
    v->rcache[0].oid=2;v->ecache_oid=2;v->wcache_oid=2;v->icache_valid=1;
}
int main(void){
    struct embk_block_device dev={.name="test"};
    struct embkfs_volume *v=calloc(1,sizeof *v);assert(v);v->dev=&dev;v->block_size=4096;v->total_blocks=64;v->root.block=17;
    fixture(v);assert(!embkfs_check(v) && data_reads==1 && !depth);
    fixture(v);data_error=-EMBK_EIO;assert(embkfs_check(v)==-EMBK_EIO);data_error=0;
    fixture(v);((struct embk_item_header *)(leaf+40))->offset=4095;assert(embkfs_check(v)==-EMBK_EINVAL);
    fixture(v);((struct embk_node_header *)leaf)->level=1;assert(embkfs_check(v)==-EMBK_EINVAL);
    fixture(v);((struct embk_internal_slot *)(root_node+40))->ptr.block=17;assert(embkfs_check(v)==-EMBK_EINVAL);
    fixture(v);root_error=-EMBK_ENOENT;assert(embkfs_check(v)==-EMBK_ENOENT);free(v);
    puts("recovery scan: cold reads, root inode, metadata bounds, tree levels/cycles and data errors OK");
}
