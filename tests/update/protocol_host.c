#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "update/protocol.h"
static unsigned checks;
#define CHECK(v) do {checks++;assert(v);}while(0)
static struct update_state factory(void) {
    struct update_state s={.revision=1,.active=0,.pending=UPDATE_NO_SLOT,.previous=UPDATE_NO_SLOT};
    s.data_guid[0]=3;s.esp_guid[0]=4;s.slot[0].root_guid[0]=1;s.slot[1].root_guid[0]=2;
    s.slot[0].release=1;s.slot[0].root_bytes=128ULL<<20;s.slot[0].efi_bytes=4096;
    strcpy(s.slot[0].version,"1.0.0");s.slot[0].efi_sha256[0]=1;s.slot[0].root_sha256[0]=2;return s;
}
static struct update_slot candidate(struct update_state s) {
    struct update_slot n=s.slot[0];memcpy(n.root_guid,s.slot[1].root_guid,16);
    n.release=2;strcpy(n.version,"2.0.0");n.efi_sha256[0]=7;n.root_sha256[0]=8;return n;
}
static void crc(uint8_t *p) {uint32_t n=update_crc32(p,508);for(unsigned i=0;i<4;i++)p[508+i]=(uint8_t)(n>>(8*i));}
int main(void) {
    CHECK(update_crc32("123456789",9)==0xcbf43926u);
    struct update_state s=factory(),decoded,base=s;uint8_t a[512],b[512],bad[512];
    CHECK(update_state_valid(&s));CHECK(update_state_encode(&s,a)==0);CHECK(update_state_decode(a,512,&decoded)==0);
    CHECK(decoded.revision==1 && decoded.active==0 && decoded.pending==UPDATE_NO_SLOT && decoded.slot[0].release==1);
    for(unsigned n=0;n<512;n++)CHECK(update_state_decode(a,n,&decoded)==UPDATE_INVALID);
    CHECK(update_state_decode(a,513,&decoded)==UPDATE_INVALID);
    CHECK(update_state_decode(NULL,512,&decoded)==UPDATE_INVALID && update_state_decode(a,512,NULL)==UPDATE_INVALID);
    memcpy(b,a,512);CHECK(update_state_choose(a,b,&decoded)==0);
    struct update_slot n=candidate(s);CHECK(update_state_stage(&s,1,&n)==0 && s.active==0 && s.pending==1 && s.attempts==1);
    CHECK(update_state_encode(&s,b)==0 && update_state_choose(a,b,&decoded)==1 && decoded.revision==2);
    for(unsigned byte=0;byte<512;byte++)for(unsigned bit=0;bit<8;bit++) {
        memcpy(bad,b,512);bad[byte]^=1u<<bit;
        CHECK(update_state_decode(bad,512,&decoded)==UPDATE_INVALID);
        CHECK(update_state_choose(a,bad,&decoded)==0 && decoded.active==0 && decoded.pending==UPDATE_NO_SLOT);
    }
    for(unsigned cut=0;cut<512;cut++) {
        memset(bad,0xa5,512);memcpy(bad,b,cut);
        CHECK(update_state_choose(a,bad,&decoded)==0 && decoded.active==0);
    }
    memcpy(bad,b,512);bad[29]=0;crc(bad);CHECK(update_state_decode(bad,512,&decoded)==UPDATE_INVALID);
    memcpy(bad,b,512);bad[31]=2;crc(bad);CHECK(update_state_decode(bad,512,&decoded)==UPDATE_INVALID);
    memcpy(bad,b,512);bad[340]=1;crc(bad);CHECK(update_state_decode(bad,512,&decoded)==UPDATE_INVALID);
    memcpy(bad,b,512);bad[8]=2;crc(bad);CHECK(update_state_decode(bad,512,&decoded)==UPDATE_INVALID);
    memcpy(bad,b,512);memcpy(bad+32,bad+48,16);crc(bad);CHECK(update_state_decode(bad,512,&decoded)==UPDATE_INVALID);
    memcpy(bad,b,512);bad[64+136+40]='/';crc(bad);CHECK(update_state_decode(bad,512,&decoded)==UPDATE_INVALID);
    memcpy(bad,b,512);bad[64+136+71]='x';crc(bad);CHECK(update_state_decode(bad,512,&decoded)==UPDATE_INVALID);
    decoded=s;decoded.slot[1].efi_sha256[1]=3;CHECK(update_state_encode(&decoded,bad)==0);
    CHECK(update_state_choose(b,bad,&decoded)==UPDATE_INVALID); /* equal sequence, contradictory records */
    unsigned selected=99;int changed=0;
    CHECK(update_state_boot(&s,&selected,&changed)==0 && selected==1 && changed==1 && s.attempts==0 && s.active==0);
    CHECK(update_state_confirm(&s,0)==UPDATE_NOT_TRIAL);
    struct update_state failed=s;
    CHECK(update_state_boot(&failed,&selected,&changed)==0 && selected==0 && failed.pending==UPDATE_NO_SLOT);
    CHECK(failed.flags==UPDATE_REVERTED && failed.last_error==UPDATE_BOOT_FAILED && failed.data_guid[0]==3);
    CHECK(update_state_boot(&failed,&selected,&changed)==0 && selected==0 && changed==0);
    CHECK(update_state_confirm(&s,1)==0 && s.active==1 && s.previous==0 && s.pending==UPDATE_NO_SLOT && s.data_guid[0]==3);
    CHECK(update_state_rollback(&s)==0 && s.active==1 && s.pending==0 && s.attempts==1);
    CHECK(update_state_boot(&s,&selected,&changed)==0 && selected==0);
    CHECK(update_state_confirm(&s,0)==0 && s.active==0 && s.previous==1 && s.data_guid[0]==3);
    CHECK(update_state_stage(&s,1,&n)==0);CHECK(update_state_stage(&s,1,&n)==UPDATE_BUSY);
    CHECK(update_state_reject(&s,UPDATE_IMAGE_FAILED)==0 && s.active==0 && s.last_error==UPDATE_IMAGE_FAILED);
    s=base;n=candidate(s);n.release=1;CHECK(update_state_stage(&s,1,&n)==UPDATE_OLDER);
    n.release=0;CHECK(update_state_stage(&s,1,&n)==UPDATE_INVALID);
    n=candidate(s);n.root_guid[0]=9;CHECK(update_state_stage(&s,1,&n)==UPDATE_INVALID);
    n=candidate(s);n.root_bytes=513;CHECK(update_state_stage(&s,1,&n)==UPDATE_INVALID);
    n=candidate(s);n.efi_bytes=UPDATE_MAX_EFI_BYTES+1;CHECK(update_state_stage(&s,1,&n)==UPDATE_INVALID);
    s.revision=UINT64_MAX;n=candidate(s);CHECK(update_state_stage(&s,1,&n)==UPDATE_EXHAUSTED && s.pending==UPDATE_NO_SLOT);
    CHECK(update_state_rollback(&base)==UPDATE_INVALID);
    s=base;s.slot[1]=candidate(s);s.slot[1].release=100;n=candidate(s);n.release=50;
    CHECK(update_state_stage(&s,1,&n)==UPDATE_OLDER); /* remembered release survives explicit rollback */
    n=s.slot[1];n.efi_sha256[0]^=1;CHECK(update_state_stage(&s,1,&n)==UPDATE_INVALID);
    n=s.slot[1];CHECK(update_state_stage(&s,1,&n)==0); /* exact verified release may be retried */
    s=base;CHECK(update_state_begin_write(&s,0)==UPDATE_INVALID);
    CHECK(update_state_begin_write(&s,1)==0 && s.active==0 && s.flags==UPDATE_WRITING);
    CHECK(update_state_begin_write(&s,1)==UPDATE_BUSY);
    CHECK(update_state_boot(&s,&selected,&changed)==0 && selected==0 && changed && s.flags==UPDATE_REVERTED && s.last_error==UPDATE_WRITE_FAILED);
    s=base;CHECK(update_state_begin_write(&s,1)==0);n=candidate(s);
    CHECK(update_state_stage(&s,1,&n)==0 && !s.flags && s.pending==1);
    memset(a,0,512);memset(b,0,512);CHECK(update_state_choose(a,b,&decoded)==UPDATE_INVALID);
    printf("PASS: %u boot journal checks: bounds, bit corruption, torn writes, trial/confirm/fallback and explicit rollback\n",checks);
}
