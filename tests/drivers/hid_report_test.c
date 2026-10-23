#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/input/hid_report.h"
static const uint8_t keyboard[]={
 0x05,1,0x09,6,0xa1,1,0x05,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
 0x75,1,0x95,8,0x81,2,0x95,1,0x75,8,0x81,1,
 0x95,6,0x75,8,0x15,0,0x25,0x65,0x19,0,0x29,0x65,0x81,0,0xc0};
static const uint8_t mouse[]={
 0x05,1,0x09,2,0xa1,1,0x85,3,0x05,9,0x19,1,0x29,3,0x15,0,0x25,1,
 0x95,3,0x75,1,0x81,2,0x95,1,0x75,5,0x81,1,
 0x05,1,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,
 0x75,8,0x95,3,0x81,6,0xc0};
static const uint8_t touch[]={
 0x05,0x0d,0x09,5,0xa1,1,0x09,0x42,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,
 0x75,7,0x81,1,0x05,1,0x09,0x30,0x09,0x31,0x15,0,0x26,0xe8,3,
 0x75,16,0x95,2,0x81,2,0xc0};
static unsigned count,usages[20],down[20],pointers;
static struct hid_pointer last;
static void key(void *ctx,uint16_t u,int d) {(void)ctx;assert(count<20);usages[count]=u;down[count++]=d;}
static void pointer(void *ctx,const struct hid_pointer *p) {(void)ctx;last=*p;pointers++;}
int main(void) {
 struct hid_report_layout l;struct hid_input_state s={0};struct hid_input_sink sink={0,key,pointer};
 assert(hid_report_parse(&l,keyboard,sizeof keyboard)==0);
 uint8_t k[]={2,0,4,0,0,0,0,0};
 assert(hid_report_decode(&l,&s,k,sizeof k,&sink)==0);
 assert(count==2 && usages[0]==0xe1 && usages[1]==4 && down[0] && down[1]);
 assert(hid_report_decode(&l,&s,k,sizeof k,&sink)==0 && count==2);
 k[2]=1;assert(hid_report_decode(&l,&s,k,sizeof k,&sink)==0 && count==2); /* rollover preserves keys */
 assert(hid_report_decode(&l,&s,k,2,&sink)==-1 && count==2);
 memset(k,0,sizeof k);assert(hid_report_decode(&l,&s,k,sizeof k,&sink)==0);
 assert(count==4 && usages[2]==0xe1 && usages[3]==4 && !down[2] && !down[3]);
 assert(hid_report_parse(&l,mouse,sizeof mouse)==0);
 uint8_t m[]={3,1,0xfb,8,0xff};
 assert(hid_report_decode(&l,&s,m,sizeof m,&sink)==0);
 assert(pointers==1 && last.buttons==1 && last.x==-5 && last.y==8 && last.wheel==-1 && !last.absolute);
 m[0]=9;assert(hid_report_decode(&l,&s,m,sizeof m,&sink)==0 && pointers==1);
 uint8_t multi[128];
 memcpy(multi,mouse,sizeof mouse-1);
 const uint8_t extra[]={0x85,4,0x05,9,0x09,1,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,
  0x75,7,0x81,1,0xc0};
 memcpy(multi+sizeof mouse-1,extra,sizeof extra);
 assert(hid_report_parse(&l,multi,sizeof mouse-1+sizeof extra)==0);
 memset(&s,0,sizeof s);
 uint8_t button[]={4,1},motion[]={3,0,1,1,0};
 assert(hid_report_decode(&l,&s,button,sizeof button,&sink)==0 && last.buttons==1);
 assert(hid_report_decode(&l,&s,motion,sizeof motion,&sink)==0 && last.buttons==1);
 button[1]=0;
 assert(hid_report_decode(&l,&s,button,sizeof button,&sink)==0 && !last.buttons);
 assert(hid_report_parse(&l,touch,sizeof touch)==0);
 uint8_t t[]={1,0xf4,1,0xe8,3};
 assert(hid_report_decode(&l,&s,t,sizeof t,&sink)==0);
 assert(last.absolute && last.touchpad && last.touching && last.x==32767 && last.y==65535);
 for(unsigned n=1;n<sizeof keyboard;n++) { /* all truncations must remain bounded */
  struct hid_report_layout tmp;(void)hid_report_parse(&tmp,keyboard,n);
 }
 uint8_t bad[]={0xb4};assert(hid_report_parse(&l,bad,sizeof bad)==-1); /* global pop underflow */
 uint8_t longitem[]={0xfe,0,0};assert(hid_report_parse(&l,longitem,sizeof longitem)==-1);
 /* Mutated real descriptors exercise item sizes, scopes and report bounds. */
 uint32_t random=0x84e349ad;
 struct hid_input_sink quiet={0};
 for(unsigned trial=0;trial<10000;trial++) {
  uint8_t mutated[sizeof keyboard],payload[64];
  memcpy(mutated,keyboard,sizeof keyboard);
  random=random*1664525U+1013904223U;
  mutated[random%sizeof mutated]^=(uint8_t)(1U<<((random>>16)%8));
  for(unsigned j=0;j<sizeof payload;j++) {random=random*1664525U+1013904223U;payload[j]=random>>24;}
  if(!hid_report_parse(&l,mutated,sizeof mutated)) {
   memset(&s,0,sizeof s);(void)hid_report_decode(&l,&s,payload,sizeof payload,&quiet);
  }
 }
 puts("HID reports: modifiers/edges, rollover, truncation, report IDs, signed mouse and touchpad passed");
}
