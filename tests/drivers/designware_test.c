#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "drivers/i2c/designware.h"
struct model {
    unsigned enabled, raw, fifo[2], head, tail, used;
    unsigned queued[2], qhead, qtail, qused, commands[256], count, reads;
    unsigned fail, silent, stopped, address;
    uint64_t now;
};
static uint32_t read_reg(void *ctx,uint32_t r) {
    struct model *m=ctx;
    switch(r) {
    case 0x9c:case 0x6c:return m->enabled;
    case 0x70:return (m->qused<2?2:0)|(m->used?8:0);
    case 0x34:return m->raw;
    case 0x40:m->raw=0;return 0;
    case 0x54:m->raw&=~64;return 0;
    case 0x60:m->raw&=~512;return 0;
    case 0x80:return 1;
    case 0x10: {
        assert(m->used); unsigned v=m->fifo[m->head++%2];m->used--;return v;
    }
    default:assert(0);return 0;
    }
}
static void write_reg(void *ctx,uint32_t r,uint32_t v) {
    struct model *m=ctx;
    switch(r) {
    case 0x6c:m->enabled=v==1; if(v==3)m->qused=0;break;
    case 0x04:assert(!m->enabled);m->address=v;break;
    case 0x10:
        assert(m->enabled && m->qused<2 && m->count<256 && !m->stopped);
        m->commands[m->count++]=v;m->queued[m->qtail++%2]=v;m->qused++;break;
    default:assert(0);
    }
}
static uint64_t now(void *ctx) {return ((struct model *)ctx)->now;}
static void pause_dw(void *ctx) {
    struct model *m=ctx;m->now+=1000;
    if(m->qused && !m->silent) {
        unsigned v=m->queued[m->qhead++%2];m->qused--;
        if(m->fail) {m->raw|=64;return;}
        if(v&256) {assert(m->used<2);m->fifo[m->tail++%2]=(m->reads++ ^ 0xa5)&255;m->used++;}
        if(v&512) {m->raw|=512;m->stopped=1;}
    }
}
static struct dw_i2c_io io(struct model *m) {return(struct dw_i2c_io){m,read_reg,write_reg,now,pause_dw,2,0};}
int main(void) {
    struct model m={0};struct dw_i2c_io b=io(&m);uint8_t w[]={0x12,0x34},r[100];
    assert(dw_i2c_transfer(&b,0x2c,w,2,r,sizeof r)==0);
    assert(m.address==0x2c && m.count==102 && !m.enabled);
    assert(m.commands[0]==0x12 && m.commands[1]==0x34);
    assert(m.commands[2]==(256|1024));assert(m.commands[101]==(256|512));
    for(unsigned i=0;i<sizeof r;i++) assert(r[i]==(uint8_t)(i^0xa5));
    memset(&m,0,sizeof m);b=io(&m);
    assert(dw_i2c_transfer(&b,0x2c,w,2,0,0)==0 && m.commands[1]==(0x34|512));
    memset(&m,0,sizeof m);b=io(&m);
    assert(dw_i2c_transfer(&b,0x2c,0,0,r,1)==0 && m.commands[0]==(256|512));
    memset(&m,0,sizeof m);b=io(&m);m.fail=1;
    assert(dw_i2c_transfer(&b,0x2c,w,2,0,0)==-2 && b.abort_source==1 && !m.enabled);
    memset(&m,0,sizeof m);b=io(&m);m.silent=1;
    assert(dw_i2c_transfer(&b,0x2c,w,2,0,0)==-1 && !m.enabled && m.now>=1000000);
    memset(&m,0,sizeof m);b=io(&m);
    assert(dw_i2c_transfer(&b,0x7f,w,2,0,0)==-3 && !m.count);
    puts("DesignWare: combined/repeated-start transfers, FIFO bounds, STOP, NACK and timeout passed");
}
