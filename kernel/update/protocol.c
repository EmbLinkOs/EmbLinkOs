#include "protocol.h"

static const uint8_t magic[8]={'E','M','U','P','S','T','1',0};
static void copy(void *to,const void *from,size_t n) {
    uint8_t *d=to;const uint8_t *s=from;while(n--)*d++=*s++;
}
static void zero(void *to,size_t n) {uint8_t *d=to;while(n--)*d++=0;}
static int equal(const void *a,const void *b,size_t n) {
    const uint8_t *x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1;
}
static int nonzero(const void *a,size_t n) {
    const uint8_t *p=a;while(n--)if(*p++)return 1;return 0;
}
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void put64(uint8_t *p,uint64_t v) {for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t get32(const uint8_t *p) {uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static uint64_t get64(const uint8_t *p) {uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
uint32_t update_crc32(const void *data,size_t n) {
    const uint8_t *p=data;uint32_t crc=~0u;
    while(n--){crc^=*p++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^((0u-(crc&1))&0xedb88320u);}
    return ~crc;
}
static int slot_valid(const struct update_slot *s,int required) {
    if(!nonzero(s->root_guid,16))return 0;
    if(!s->release)return !required && !s->root_bytes && !s->efi_bytes &&
        !nonzero(s->version,sizeof s->version) && !nonzero(s->efi_sha256,32) && !nonzero(s->root_sha256,32);
    if(!s->root_bytes || s->root_bytes>UPDATE_MAX_ROOT_BYTES || (s->root_bytes&511) ||
       !s->efi_bytes || s->efi_bytes>UPDATE_MAX_EFI_BYTES ||
       !nonzero(s->efi_sha256,32) || !nonzero(s->root_sha256,32) || !s->version[0])return 0;
    unsigned i=0;for(;i<UPDATE_VERSION_BYTES && s->version[i];i++) {
        unsigned char c=(unsigned char)s->version[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='.' || c=='-' || c=='_'))return 0;
    }
    if(i==UPDATE_VERSION_BYTES)return 0;
    for(;i<UPDATE_VERSION_BYTES;i++)if(s->version[i])return 0;
    return 1;
}
int update_state_valid(const struct update_state *s) {
    if(!s || !s->revision || s->active>1 || (s->flags&~(UPDATE_REVERTED|UPDATE_WRITING)) || s->flags==(UPDATE_REVERTED|UPDATE_WRITING) ||
       !nonzero(s->data_guid,16) || !nonzero(s->esp_guid,16) || equal(s->data_guid,s->esp_guid,16))return 0;
    for(unsigned i=0;i<2;i++)if(!slot_valid(&s->slot[i],i==s->active) ||
        equal(s->slot[i].root_guid,s->data_guid,16) || equal(s->slot[i].root_guid,s->esp_guid,16))return 0;
    if(equal(s->slot[0].root_guid,s->slot[1].root_guid,16))return 0;
    if(s->pending!=UPDATE_NO_SLOT && (s->pending>1 || s->pending==s->active ||
       !slot_valid(&s->slot[s->pending],1) || s->attempts>1))return 0;
    if(s->pending==UPDATE_NO_SLOT && s->attempts)return 0;
    if((s->flags&UPDATE_REVERTED) && (s->pending!=UPDATE_NO_SLOT || !s->last_error))return 0;
    if(!(s->flags&UPDATE_REVERTED) && s->last_error)return 0;
    if((s->flags&UPDATE_WRITING) && (s->pending!=UPDATE_NO_SLOT || s->previous!=UPDATE_NO_SLOT))return 0;
    if(s->previous!=UPDATE_NO_SLOT && (s->previous>1 || s->previous==s->active || !slot_valid(&s->slot[s->previous],1)))return 0;
    return s->last_error<=UPDATE_WRITE_FAILED;
}
int update_state_encode(const struct update_state *s,uint8_t out[UPDATE_RECORD_BYTES]) {
    if(!out || !update_state_valid(s))return UPDATE_INVALID;
    zero(out,UPDATE_RECORD_BYTES);copy(out,magic,8);put32(out+8,1);put32(out+12,UPDATE_RECORD_BYTES);
    put64(out+16,s->revision);put32(out+24,s->flags);
    out[28]=s->active;out[29]=s->pending;out[30]=s->previous;out[31]=s->attempts;
    copy(out+32,s->data_guid,16);copy(out+48,s->esp_guid,16);
    for(unsigned i=0;i<2;i++) {
        uint8_t *p=out+64+i*136;const struct update_slot *slot=&s->slot[i];
        copy(p,slot->root_guid,16);put64(p+16,slot->release);put64(p+24,slot->root_bytes);put64(p+32,slot->efi_bytes);
        copy(p+40,slot->version,32);copy(p+72,slot->efi_sha256,32);copy(p+104,slot->root_sha256,32);
    }
    put32(out+336,s->last_error);put32(out+508,update_crc32(out,508));return UPDATE_OK;
}
int update_state_decode(const uint8_t *p,size_t n,struct update_state *out) {
    if(!p || !out || n!=UPDATE_RECORD_BYTES || !equal(p,magic,8) || get32(p+8)!=1 ||
       get32(p+12)!=UPDATE_RECORD_BYTES || get32(p+508)!=update_crc32(p,508) || nonzero(p+340,168))return UPDATE_INVALID;
    struct update_state s;zero(&s,sizeof s);s.revision=get64(p+16);s.flags=get32(p+24);
    s.active=p[28];s.pending=p[29];s.previous=p[30];s.attempts=p[31];
    copy(s.data_guid,p+32,16);copy(s.esp_guid,p+48,16);
    for(unsigned i=0;i<2;i++) {
        const uint8_t *q=p+64+i*136;struct update_slot *slot=&s.slot[i];
        copy(slot->root_guid,q,16);slot->release=get64(q+16);slot->root_bytes=get64(q+24);slot->efi_bytes=get64(q+32);
        copy(slot->version,q+40,32);copy(slot->efi_sha256,q+72,32);copy(slot->root_sha256,q+104,32);
    }
    s.last_error=get32(p+336);if(!update_state_valid(&s))return UPDATE_INVALID;*out=s;return UPDATE_OK;
}
int update_state_choose(const uint8_t *a,const uint8_t *b,struct update_state *out) {
    struct update_state x,y;int ax=update_state_decode(a,UPDATE_RECORD_BYTES,&x)==0;
    int by=update_state_decode(b,UPDATE_RECORD_BYTES,&y)==0;
    if(!out || (!ax && !by))return UPDATE_INVALID;
    if(ax && by && x.revision==y.revision && !equal(a,b,UPDATE_RECORD_BYTES))return UPDATE_INVALID;
    if(ax && (!by || x.revision>=y.revision)){*out=x;return 0;}*out=y;return 1;
}
static int bump(struct update_state *s) {
    if(s->revision==UINT64_MAX)return UPDATE_EXHAUSTED;
    s->revision++;return UPDATE_OK;
}
int update_state_begin_write(struct update_state *s,unsigned inactive) {
    if(!update_state_valid(s) || inactive>1 || inactive==s->active)return UPDATE_INVALID;
    if(s->pending!=UPDATE_NO_SLOT || (s->flags&UPDATE_WRITING))return UPDATE_BUSY;
    if(s->revision==UINT64_MAX)return UPDATE_EXHAUSTED;
    s->previous=UPDATE_NO_SLOT;s->flags=UPDATE_WRITING;s->last_error=0;return bump(s);
}
int update_state_stage(struct update_state *s,unsigned slot,const struct update_slot *candidate) {
    if(!update_state_valid(s) || !candidate || slot>1 || slot==s->active || !slot_valid(candidate,1) ||
       !equal(s->slot[slot].root_guid,candidate->root_guid,16))return UPDATE_INVALID;
    if(s->pending!=UPDATE_NO_SLOT)return UPDATE_BUSY;
    if(candidate->release<=s->slot[s->active].release)return UPDATE_OLDER;
    const struct update_slot *known=&s->slot[slot];
    if(candidate->release<known->release)return UPDATE_OLDER;
    if(candidate->release==known->release &&
       (candidate->root_bytes!=known->root_bytes || candidate->efi_bytes!=known->efi_bytes ||
        !equal(candidate->version,known->version,UPDATE_VERSION_BYTES) ||
        !equal(candidate->efi_sha256,known->efi_sha256,32) || !equal(candidate->root_sha256,known->root_sha256,32)))return UPDATE_INVALID;
    if(s->revision==UINT64_MAX)return UPDATE_EXHAUSTED;
    s->slot[slot]=*candidate;s->pending=(uint8_t)slot;s->previous=UPDATE_NO_SLOT;s->attempts=1;
    s->flags=0;s->last_error=0;return bump(s);
}
int update_state_boot(struct update_state *s,unsigned *slot,int *changed) {
    if(!update_state_valid(s) || !slot || !changed)return UPDATE_INVALID;
    *slot=s->active;*changed=0;
    if(s->flags&UPDATE_WRITING){*changed=1;return update_state_reject(s,UPDATE_WRITE_FAILED);}
    if(s->pending==UPDATE_NO_SLOT)return UPDATE_OK;
    if(s->revision==UINT64_MAX)return UPDATE_EXHAUSTED;
    *changed=1;
    if(s->attempts){*slot=s->pending;s->attempts=0;return bump(s);}
    return update_state_reject(s,UPDATE_BOOT_FAILED);
}
int update_state_reject(struct update_state *s,uint32_t reason) {
    if(!update_state_valid(s) || reason<UPDATE_BOOT_FAILED || reason>UPDATE_WRITE_FAILED)return UPDATE_INVALID;
    if(s->pending==UPDATE_NO_SLOT && !(s->flags&UPDATE_WRITING))return UPDATE_NOT_TRIAL;
    if(s->revision==UINT64_MAX)return UPDATE_EXHAUSTED;
    s->pending=UPDATE_NO_SLOT;s->attempts=0;s->flags=UPDATE_REVERTED;s->last_error=reason;return bump(s);
}
int update_state_confirm(struct update_state *s,unsigned running) {
    if(!update_state_valid(s) || running>1)return UPDATE_INVALID;
    if(s->pending!=running || s->attempts)return UPDATE_NOT_TRIAL;
    if(s->revision==UINT64_MAX)return UPDATE_EXHAUSTED;
    s->previous=s->active;s->active=(uint8_t)running;s->pending=UPDATE_NO_SLOT;s->flags=0;s->last_error=0;return bump(s);
}
int update_state_rollback(struct update_state *s) {
    if(!update_state_valid(s) || s->previous==UPDATE_NO_SLOT)return UPDATE_INVALID;
    if(s->pending!=UPDATE_NO_SLOT)return UPDATE_BUSY;
    if(s->revision==UINT64_MAX)return UPDATE_EXHAUSTED;
    s->pending=s->previous;s->previous=UPDATE_NO_SLOT;s->attempts=1;s->flags=0;s->last_error=0;return bump(s);
}
