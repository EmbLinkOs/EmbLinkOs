#include "auth.h"
#include "embk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUTH_ITERATIONS 4096u
#define SHA256_BLOCK 64
#define SHA256_SIZE 32

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[SHA256_BLOCK];
    uint32_t buf_len;
};

static const uint32_t k256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void transform(struct sha256_ctx *c, const uint8_t b[64]) {
    uint32_t w[64];
    for (int i=0;i<16;i++) w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for (int i=16;i<64;i++) {
        uint32_t a=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
        uint32_t z=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+a+w[i-7]+z;
    }
    uint32_t a=c->state[0],b0=c->state[1],d0=c->state[2],d=c->state[3];
    uint32_t e=c->state[4],f=c->state[5],g=c->state[6],h=c->state[7];
    for(int i=0;i<64;i++){
        uint32_t s1=rotr(e,6)^rotr(e,11)^rotr(e,25), ch=(e&f)^((~e)&g);
        uint32_t t1=h+s1+ch+k256[i]+w[i], s0=rotr(a,2)^rotr(a,13)^rotr(a,22);
        uint32_t t2=s0+((a&b0)^(a&d0)^(b0&d0));
        h=g;g=f;f=e;e=d+t1;d=d0;d0=b0;b0=a;a=t1+t2;
    }
    c->state[0]+=a;c->state[1]+=b0;c->state[2]+=d0;c->state[3]+=d;
    c->state[4]+=e;c->state[5]+=f;c->state[6]+=g;c->state[7]+=h;
}

static void sha_init(struct sha256_ctx *c) {
    static const uint32_t s[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    memcpy(c->state,s,sizeof s); c->bitlen=0; c->buf_len=0;
}
static void sha_update(struct sha256_ctx *c,const void *data,size_t len) {
    const uint8_t *p=data;
    while(len){size_t n=64-c->buf_len;if(n>len)n=len;memcpy(c->buf+c->buf_len,p,n);c->buf_len+=(uint32_t)n;c->bitlen+=(uint64_t)n*8;p+=n;len-=n;if(c->buf_len==64){transform(c,c->buf);c->buf_len=0;}}
}
static void sha_final(struct sha256_ctx *c,uint8_t out[32]) {
    uint64_t bits=c->bitlen;uint8_t x=0x80,z=0;sha_update(c,&x,1);while(c->buf_len!=56)sha_update(c,&z,1);
    uint8_t lb[8];for(int i=0;i<8;i++)lb[i]=(uint8_t)(bits>>(56-i*8));sha_update(c,lb,8);
    for(int i=0;i<8;i++){out[i*4]=(uint8_t)(c->state[i]>>24);out[i*4+1]=(uint8_t)(c->state[i]>>16);out[i*4+2]=(uint8_t)(c->state[i]>>8);out[i*4+3]=(uint8_t)c->state[i];}
}
static void sha(const void *p,size_t n,uint8_t out[32]){struct sha256_ctx c;sha_init(&c);sha_update(&c,p,n);sha_final(&c,out);}
static void hmac(const uint8_t *key,size_t kn,const uint8_t *msg,size_t mn,uint8_t out[32]){
    uint8_t kb[64]={0},ip[64],op[64],inner[32];if(kn>64)sha(key,kn,kb);else memcpy(kb,key,kn);
    for(int i=0;i<64;i++){ip[i]=kb[i]^0x36;op[i]=kb[i]^0x5c;}struct sha256_ctx c;sha_init(&c);sha_update(&c,ip,64);sha_update(&c,msg,mn);sha_final(&c,inner);sha_init(&c);sha_update(&c,op,64);sha_update(&c,inner,32);sha_final(&c,out);
}
static void derive(const char *password,const uint8_t salt[16],uint32_t iterations,uint8_t out[32]){
    uint8_t block[20],u[32];memcpy(block,salt,16);block[16]=0;block[17]=0;block[18]=0;block[19]=1;
    hmac((const uint8_t*)password,strlen(password),block,sizeof block,u);memcpy(out,u,32);
    for(uint32_t j=1;j<iterations;j++){hmac((const uint8_t*)password,strlen(password),u,32,u);for(int i=0;i<32;i++)out[i]^=u[i];}
}
static int read_text(const char *path,char *buf,size_t cap){
    int fd=(int)embk_open(path,EMBK_O_RDONLY,0);if(fd<0)return -1;size_t off=0;
    while(off+1<cap){int64_t n=embk_read(fd,buf+off,cap-1-off);if(n<=0)break;off+=(size_t)n;}
    embk_close(fd);buf[off]=0;return (int)off;
}
static int append_text(const char *path,const char *line){
    int fd=(int)embk_open(path,EMBK_O_WRONLY,0);if(fd<0)return -1;
    if(embk_lseek(fd,0,EMBK_SEEK_END)<0){embk_close(fd);return -1;}
    size_t len=strlen(line),off=0;while(off<len){int64_t n=embk_write(fd,line+off,len-off);if(n<=0){embk_close(fd);return -1;}off+=(size_t)n;}
    embk_close(fd);return 0;
}
static char hex_digit(unsigned v){return (char)(v<10?'0'+v:'a'+v-10);}
static void to_hex(const uint8_t *in,size_t n,char *out){
    for(size_t i=0;i<n;i++){out[i*2]=hex_digit(in[i]>>4);out[i*2+1]=hex_digit(in[i]&15);}out[n*2]=0;
}
static int hex_value(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static int from_hex(const char *in,size_t n,uint8_t *out){
    for(size_t i=0;i<n;i++){int a=hex_value(in[i*2]),b=hex_value(in[i*2+1]);if(a<0||b<0)return -1;out[i]=(uint8_t)((a<<4)|b);}return 0;
}
static const char *shadow_record(const char *text,const char *u){
    size_t un=strlen(u);const char *p=text;
    while(*p){if(!memcmp(p,u,un)&&p[un]==':')return p+un+1;while(*p&&*p!='\n')p++;if(*p)p++;}
    return NULL;
}
int embk_auth_valid_username(const char *u){
    size_t n=strlen(u);if(!n||n>EMBK_AUTH_USERNAME_MAX)return 0;
    for(size_t i=0;i<n;i++)if(!((u[i]>='a'&&u[i]<='z')||(u[i]>='0'&&u[i]<='9')||u[i]=='_'||u[i]=='-'))return 0;
    return 1;
}
int embk_auth_has_accounts(void){
    struct embk_stat st;return embk_stat(EMBK_AUTH_SHADOW,&st)>=0&&st.size>0;
}
int embk_auth_create(const char *u,const char *password){
    if(!embk_auth_valid_username(u)||strlen(password)<8||strlen(password)>EMBK_AUTH_PASSWORD_MAX)return -1;
    char shadow[8192];int shadow_len=read_text(EMBK_AUTH_SHADOW,shadow,sizeof shadow);if(shadow_len<0)return -4;
    if(shadow_record(shadow,u))return -2;
    uint8_t salt[16],digest[32];if(getentropy(salt,sizeof salt)!=0)return -3;
    derive(password,salt,AUTH_ITERATIONS,digest);
    char salt_hex[33],digest_hex[65],line[192],passwd_line[192];
    to_hex(salt,sizeof salt,salt_hex);to_hex(digest,sizeof digest,digest_hex);
    snprintf(line,sizeof line,"%s:$pbkdf2-sha256$%u$%s$%s:0:0:99999:7:::\n",
             u,AUTH_ITERATIONS,salt_hex,digest_hex);
    int users=0;for(int i=0;i<shadow_len;i++)if(shadow[i]=='\n')users++;
    snprintf(passwd_line,sizeof passwd_line,"%s:x:%d:%d:%s:/home/%s:/system/bin/shell.elf\n",
             u,1000+users,1000+users,u,u);
    if(append_text(EMBK_AUTH_PASSWD,passwd_line)<0)return -4;
    if(append_text(EMBK_AUTH_SHADOW,line)<0)return -5;
    memset(salt,0,sizeof salt);memset(digest,0,sizeof digest);memset(shadow,0,sizeof shadow);return 0;
}
int embk_auth_verify(const char *u,const char *password){
    if(!embk_auth_valid_username(u)||strlen(password)>EMBK_AUTH_PASSWORD_MAX)return 0;
    char shadow[8192];if(read_text(EMBK_AUTH_SHADOW,shadow,sizeof shadow)<0)return 0;
    const char *p=shadow_record(shadow,u),*prefix="$pbkdf2-sha256$";if(!p||memcmp(p,prefix,15)!=0)return 0;p+=15;
    char *end;uint32_t iterations=(uint32_t)strtoul(p,&end,10);if(!iterations||*end!='$')return 0;p=end+1;
    if(strlen(p)<33||p[32]!='$')return 0;
    uint8_t salt[16],expected[32],got[32];
    if(from_hex(p,16,salt)<0)return 0;
    p+=33;
    if(strlen(p)<64||from_hex(p,32,expected)<0)return 0;
    derive(password,salt,iterations,got);uint8_t diff=0;for(int i=0;i<32;i++)diff|=got[i]^expected[i];
    memset(got,0,sizeof got);memset(expected,0,sizeof expected);memset(shadow,0,sizeof shadow);return diff==0;
}
