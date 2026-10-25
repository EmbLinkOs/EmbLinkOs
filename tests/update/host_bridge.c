#include "update/protocol.h"
#include "release.h"
#include "ecdsa.h"
#include "crypto/sha256.h"
int journal_roundtrip(const unsigned char *in,unsigned char *out) {
    struct update_state state;
    if(update_state_decode(in,512,&state))return -1;
    return update_state_encode(&state,out);
}
int release_verify(const unsigned char *p,unsigned n,const unsigned char *key) {
    struct embk_update_image image;char notes[256];unsigned char hash[32];
    if(update_release_parse(p,n,&image,notes))return -1;
    sha256(p,RELEASE_SIGNED_BYTES,hash);
    return ecdsa_verify(ec_p256(),key,key+32,hash,32,p+384,32,p+416,32)?0:-1;
}
/* SHA-256's optional console selftest is linked but unused on the host. */
void kprintf(const char *format,...) {(void)format;}
