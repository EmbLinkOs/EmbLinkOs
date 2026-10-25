#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/i2c/hid_protocol.h"
struct model {unsigned stage,nacks,bad_version,no_reset,input_length;uint64_t now;};
static void le16(uint8_t *p,unsigned n){p[0]=n;p[1]=n>>8;}
static int xfer(void *ctx,const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn) {
 struct model *m=ctx;
 switch(m->stage) {
 case 0:
  assert(wn==2 && w[0]==0x10 && !w[1] && rn==30);memset(r,0,rn);
  le16(r,30);le16(r+2,m->bad_version?0x200:0x100);le16(r+4,4);le16(r+6,0x30);
  le16(r+8,0x40);le16(r+10,10);le16(r+16,0x20);le16(r+20,0x1234);le16(r+22,0xabcd);
  m->stage++;return 0;
 case 1:case 2:
  assert(wn==4 && !rn && w[0]==0x20 && !w[1] && !w[2] && w[3]==(m->stage==1?8:1));
  m->stage++;return 0;
 case 3:
  assert(!wn && rn==10);
  if(m->no_reset || m->nacks++<2) return -2;
  memset(r,0,rn);m->stage++;return 0;
 case 4:
  assert(wn==2 && w[0]==0x30 && !w[1] && rn==4);
  memcpy(r,"HID!",4);m->stage++;return 0;
 case 5:
  assert(!wn && rn==10);memset(r,0,rn);le16(r,m->input_length);return 0;
 default:assert(0);return -1;
 }
}
static uint64_t now(void *ctx){return ((struct model *)ctx)->now;}
static void sleep_ms(void *ctx,unsigned ms){((struct model *)ctx)->now+=ms;}
int main(void) {
 struct model m={0};struct i2c_hid_transport t={&m,xfer,now,sleep_ms};
 struct i2c_hid_descriptor d;uint8_t report[10],input[10];
 assert(i2c_hid_start(&t,0x10,&d,report,sizeof report)==0);
 assert(d.vendor==0x1234 && d.product==0xabcd && !memcmp(report,"HID!",4));
 m.input_length=8;assert(i2c_hid_read(&t,&d,input,sizeof input)==6);
 m.input_length=0xffff;assert(i2c_hid_read(&t,&d,input,sizeof input)==0);
 m.input_length=2;assert(i2c_hid_read(&t,&d,input,sizeof input)==0);
 m.input_length=11;assert(i2c_hid_read(&t,&d,input,sizeof input)==-3);
 m.input_length=0;assert(i2c_hid_read(&t,&d,input,sizeof input)==-4);
 assert(i2c_hid_read(&t,&d,input,1)==-3);
 memset(&m,0,sizeof m);m.bad_version=1;
 assert(i2c_hid_start(&t,0x10,&d,report,sizeof report)==-3 && m.stage==1);
 memset(&m,0,sizeof m);m.no_reset=1;
 assert(i2c_hid_start(&t,0x10,&d,report,sizeof report)==-1 && m.now>=1000 && m.stage==3);
 puts("I2C HID: descriptor, power/reset commands, delayed completion, input lengths and timeout passed");
}
