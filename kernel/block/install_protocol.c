#include "block/install.h"
#include "update/protocol.h"
#include "include/errno.h"
#include "include/kstring.h"

static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t r64(const uint8_t *p) {return r32(p) | (uint64_t)r32(p+4)<<32;}
static void w32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++) p[i]=v>>(8*i);}
static void w64(uint8_t *p,uint64_t v) {for(unsigned i=0;i<8;i++) p[i]=v>>(8*i);}
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t v=~0u;
    while(n--) {v^=*p++;for(unsigned i=0;i<8;i++) v=(v>>1)^(0xedb88320u & (0u-(v&1)));}
    return ~v;
}
static int zero(const uint8_t *p,size_t n) {while(n--) if(*p++) return 0;return 1;}
static uint32_t crc32c(const uint8_t *p,size_t n) {
    uint32_t v=~0u;
    while(n--) {v^=*p++;for(unsigned i=0;i<8;i++)v=(v>>1)^(0x82f63b78u&(0u-(v&1)));}
    return ~v;
}
static int header_ok(uint8_t *h,uint64_t here,uint64_t other) {
    if(memcmp(h,"EFI PART",8) || r32(h+8)!=0x10000 || r32(h+12)!=92 ||
       r32(h+20) || r64(h+24)!=here || r64(h+32)!=other ||
       r32(h+80)!=128 || r32(h+84)!=128) return 0;
    uint32_t expected=r32(h+16);w32(h+16,0);
    uint32_t got=crc(h,92);w32(h+16,expected);return expected==got;
}
int install_prepare(const struct install_io *src,const struct install_io *dst,struct install_plan *p) {
    if(!src || !dst || !p || !src->read || !dst->read || !dst->write || !dst->flush ||
       src->ctx==dst->ctx || src->sectors<4096 || dst->sectors<4096)
        return -EMBK_EINVAL;
    memset(p,0,sizeof *p);
    if(src->read(src->ctx,0,1,p->mbr) || src->read(src->ctx,1,1,p->header)) return -EMBK_EIO;
    /* A flashed image usually occupies only the beginning of a larger USB
     * device. Validate its declared GPT geometry, not the unused media tail. */
    uint64_t alternate=r64(p->header+32);
    if(alternate<4095 || alternate>=src->sectors)return -EMBK_EINVAL;
    uint64_t declared=alternate+1;
    if(dst->sectors<declared)return -EMBK_EINVAL;
    p->source_gpt_sectors=declared;
    if(p->mbr[510]!=0x55 || p->mbr[511]!=0xaa || p->mbr[450]!=0xee ||
       r32(p->mbr+454)!=1 || !zero(p->mbr+462,48) ||
       !header_ok(p->header,1,declared-1) || r64(p->header+72)!=2 ||
       r64(p->header+40)<34 || r64(p->header+48)!=declared-34)
        return -EMBK_EINVAL;
    if(src->read(src->ctx,2,32,p->entries) || crc(p->entries,sizeof p->entries)!=r32(p->header+88))
        return -EMBK_EIO;
    /* Independent backup header and array, not merely a primary CRC check. */
    uint8_t backup[512];
    if(src->read(src->ctx,declared-1,1,backup) ||
       !header_ok(backup,declared-1,1) || r64(backup+72)!=declared-33 ||
       memcmp(backup+40,p->header+40,32) || memcmp(backup+80,p->header+80,12)) return -EMBK_EINVAL;
    for(unsigned i=0;i<32;i++) {
        if(src->read(src->ctx,declared-33+i,1,backup) ||
           memcmp(backup,p->entries+i*512,512)) return -EMBK_EIO;
    }
    static const uint8_t esp[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
    static const uint8_t data[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    unsigned used=0;
    for(unsigned i=0;i<128;i++) {
        const uint8_t *e=p->entries+i*128;
        if(zero(e,16)) {if(!zero(e,128)) return -EMBK_EINVAL;continue;}
        uint64_t first=r64(e+32),last=r64(e+40);
        if(first<r64(p->header+40) || last<first || last>r64(p->header+48) || zero(e+16,16))
            return -EMBK_EINVAL;
        if(!memcmp(e,esp,16) && !p->esp_sectors) {p->esp_start=first;p->esp_sectors=last-first+1;}
        else if(!memcmp(e,data,16) && !p->root_sectors) {p->root_start=first;p->root_sectors=last-first+1;}
        else if(memcmp(e,data,16) || used>=4)return -EMBK_EINVAL;
        if(last+1>p->copy_end) p->copy_end=last+1;
        used++;
    }
    if(used==4) {
        /* Managed images have a deliberately fixed order: ESP, OS-A, OS-B,
         * Data. Only Data grows; neither signed OS partition is rewritten. */
        for(unsigned i=0;i<4;i++) {
            const uint8_t *entry=p->entries+i*128;
            if(memcmp(entry,i?data:esp,16))return -EMBK_EINVAL;
            for(unsigned j=0;j<i;j++) {
                const uint8_t *other=p->entries+j*128;
                if(!(r64(entry+32)>r64(other+40) || r64(other+32)>r64(entry+40)) ||
                   !memcmp(entry+16,other+16,16))return -EMBK_EINVAL;
            }
        }
        p->managed=1;p->root_start=r64(p->entries+3*128+32);
        p->root_sectors=r64(p->entries+3*128+40)-p->root_start+1;
        if(src->read(src->ctx,p->esp_start,1,backup) || memcmp(backup+3,"EMBUPD1 ",8) ||
           (backup[14]|(unsigned)backup[15]<<8)<128)return -EMBK_EINVAL;
        uint8_t records[2][512];struct update_state state;
        if(src->read(src->ctx,p->esp_start+UPDATE_CONTROL0_OFFSET/512,1,records[0]) ||
           src->read(src->ctx,p->esp_start+UPDATE_CONTROL1_OFFSET/512,1,records[1]) ||
           update_state_choose(records[0],records[1],&state)<0 ||
           state.pending!=UPDATE_NO_SLOT || (state.flags&UPDATE_WRITING) ||
           memcmp(state.esp_guid,p->entries+16,16) || memcmp(state.data_guid,p->entries+3*128+16,16) ||
           memcmp(state.slot[0].root_guid,p->entries+128+16,16) ||
           memcmp(state.slot[1].root_guid,p->entries+256+16,16))return -EMBK_EINVAL;
        /* A freshly installed copy starts at its confirmed source release. */
        state.revision=1;state.previous=UPDATE_NO_SLOT;state.flags=0;state.last_error=0;
        if(update_state_encode(&state,p->update_record))return -EMBK_EINVAL;
    } else if(used!=2)return -EMBK_EINVAL;
    if(!p->esp_sectors || !p->root_sectors ||
       !(p->esp_start+p->esp_sectors<=p->root_start || p->root_start+p->root_sectors<=p->esp_start))return -EMBK_EINVAL;
    /* Only the standard trailing root layout can grow into the target's free
     * space. Validate both filesystem superblocks before promising any writes. */
    if(p->root_start<p->esp_start+p->esp_sectors || p->root_sectors<256 ||
       src->read(src->ctx,p->root_start+128,1,backup))return -EMBK_EINVAL;
    uint64_t bs=r64(backup+40);
    if(r64(backup)!=0x373153464b424d45ULL || r32(backup+8)>2 ||
       bs<4096 || bs>65536 || (bs&(bs-1)) || p->root_sectors%(bs/512) ||
       r64(backup+48)!=p->root_sectors/(bs/512) || r64(backup+56)>r64(backup+48) ||
       r64(backup+152)!=crc32c(backup,152))return -EMBK_EINVAL;
    p->filesystem_block_size=(uint32_t)bs;p->filesystem_blocks=r64(backup+48);p->filesystem_free=r64(backup+56);
    uint8_t primary[512];memcpy(primary,backup,512);
    if(src->read(src->ctx,p->root_start+p->root_sectors-bs/512,1,backup) || memcmp(primary,backup,512))return -EMBK_EIO;
    p->target_root_sectors=(dst->sectors-33-p->root_start)/(bs/512)*(bs/512);
    if(p->target_root_sectors<p->root_sectors)return -EMBK_EINVAL;
    p->source_sectors=src->sectors;p->target_sectors=dst->sectors;
    p->fingerprint=crc(p->header,512)^crc(p->entries,sizeof p->entries)^crc(primary,512);
    return 0;
}
static int checked_write(const struct install_io *d,uint64_t lba,uint32_t n,const void *b,void *v) {
    if(d->write(d->ctx,lba,n,b) || d->flush(d->ctx) || d->read(d->ctx,lba,n,v) ||
       memcmp(b,v,(size_t)n*512)) return -EMBK_EIO;
    return 0;
}
static void seal(uint8_t *h) {w32(h+16,0);w32(h+16,crc(h,92));}
int install_copy(const struct install_io *src,const struct install_io *dst,struct install_plan *p,
                 const uint8_t ids[52],void *buffer,void *verify,size_t cap,
                 void (*progress)(uint64_t,uint64_t)) {
    if(!src || !dst || !p || !ids || !buffer || !verify || buffer==verify || cap<16384 ||
       cap>1024*1024 || !dst->write || !dst->flush || !src->read || !dst->read ||
       src->ctx==dst->ctx || p->source_sectors!=src->sectors || p->target_sectors!=dst->sectors ||
       zero(ids,16) || zero(ids+16,16) || zero(ids+32,16) || zero(ids+48,4) ||
       !memcmp(ids+16,ids+32,16)) return -EMBK_EINVAL;
    /* Revalidate the plan immediately before the first write. Caller serializes source access. */
    struct install_plan *check=(struct install_plan *)buffer;
    if(cap<sizeof *check) return -EMBK_EINVAL;
    int rc=install_prepare(src,dst,check);
    if(rc || memcmp(check,p,sizeof *p)) return rc ? rc : -EMBK_EINVAL;
    if(cap<p->filesystem_block_size)return -EMBK_EINVAL;
    uint8_t *b=buffer;
    memset(b,0,512);
    /* Invalidate old boot metadata before touching payload. Never publish a
     * new primary GPT or MBR until all payload bytes have been read back. */
    if(checked_write(dst,0,1,b,verify) || checked_write(dst,1,1,b,verify) ||
       checked_write(dst,dst->sectors-1,1,b,verify)) return -EMBK_EIO;
    uint32_t chunk=(uint32_t)(cap/512);
    for(uint64_t at=34;at<p->copy_end;) {
        uint32_t n=p->copy_end-at<chunk?(uint32_t)(p->copy_end-at):chunk;
        if(src->read(src->ctx,at,n,b) || checked_write(dst,at,n,b,verify)) return -EMBK_EIO;
        at+=n;if(progress) progress(at,p->copy_end);
    }
    memcpy(p->header+56,ids,16);
    /* Grow the copied filesystem without moving any data or tree blocks.
     * The old tail becomes free; a new tail superblock consumes one new block.
     * Hence free_blocks grows by precisely new_total - old_total. */
    uint32_t spb=p->filesystem_block_size/512;
    if(src->read(src->ctx,p->root_start+128,spb,b))return -EMBK_EIO;
    uint64_t total=p->target_root_sectors/spb;
    w64(b+48,total);w64(b+56,p->filesystem_free+total-p->filesystem_blocks);
    w64(b+152,crc32c(b,152));
    if(checked_write(dst,p->root_start+p->target_root_sectors-spb,spb,b,verify) ||
       checked_write(dst,p->root_start+128,spb,b,verify))return -EMBK_EIO;
    unsigned idx=0;uint8_t identities[4][16];
    memcpy(identities[0],ids+16,16);memcpy(identities[1],ids+32,16);
    memcpy(identities[2],ids+32,16);identities[2][15]^=0x40;
    memcpy(identities[3],ids+32,16);identities[3][15]^=0x80;
    if(p->managed) {
        for(unsigned i=0;i<4;i++) {
            if(zero(identities[i],16))return -EMBK_EINVAL;
            for(unsigned j=0;j<i;j++)if(!memcmp(identities[i],identities[j],16))return -EMBK_EINVAL;
        }
        struct update_state state;if(update_state_decode(p->update_record,512,&state))return -EMBK_EINVAL;
        memcpy(state.esp_guid,identities[0],16);memcpy(state.slot[0].root_guid,identities[1],16);
        memcpy(state.slot[1].root_guid,identities[2],16);memcpy(state.data_guid,identities[3],16);
        if(update_state_encode(&state,p->update_record))return -EMBK_EINVAL;
        const unsigned offsets[]={UPDATE_CONTROL0_OFFSET,UPDATE_CONTROL1_OFFSET,UPDATE_FACTORY_OFFSET};
        for(unsigned i=0;i<3;i++) {
            memset(b,0,4096);memcpy(b,p->update_record,512);
            if(checked_write(dst,p->esp_start+offsets[i]/512,8,b,verify))return -EMBK_EIO;
        }
    }
    for(unsigned i=0;i<128;i++) if(!zero(p->entries+i*128,16)) {
        memcpy(p->entries+i*128+16,identities[idx++],16);
        if(r64(p->entries+i*128+32)==p->root_start)
            w64(p->entries+i*128+40,p->root_start+p->target_root_sectors-1);
    }
    w32(p->header+88,crc(p->entries,sizeof p->entries));
    w64(p->header+32,dst->sectors-1);w64(p->header+48,dst->sectors-34);
    seal(p->header);
    uint8_t backup[512];memcpy(backup,p->header,512);
    w64(backup+24,dst->sectors-1);w64(backup+32,1);w64(backup+72,dst->sectors-33);seal(backup);
    memcpy(p->mbr+440,ids+48,4);
    w32(p->mbr+458,dst->sectors-1>UINT32_MAX?UINT32_MAX:(uint32_t)(dst->sectors-1));
    if(checked_write(dst,dst->sectors-33,32,p->entries,verify) ||
       checked_write(dst,dst->sectors-1,1,backup,verify) ||
       checked_write(dst,2,32,p->entries,verify) ||
       checked_write(dst,1,1,p->header,verify) ||
       checked_write(dst,0,1,p->mbr,verify)) return -EMBK_EIO;
    return 0;
}
