#include "hid_protocol.h"
static uint16_t le16(const uint8_t *p) { return p[0]|((uint16_t)p[1]<<8); }
int i2c_hid_start(struct i2c_hid_transport *t,uint16_t reg,
    struct i2c_hid_descriptor *d,uint8_t *report,unsigned capacity) {
    uint8_t address[2]={reg,reg>>8},raw[30];
    int rc=t->xfer(t->ctx,address,2,raw,sizeof raw);
    if(rc) return rc;
    if(le16(raw)!=30 || le16(raw+2)!=0x100) return -3;
    d->report_length=le16(raw+4);d->report_register=le16(raw+6);
    d->input_register=le16(raw+8);d->input_max=le16(raw+10);
    d->command_register=le16(raw+16);d->vendor=le16(raw+20);d->product=le16(raw+22);
    if(!d->report_length || d->report_length>capacity || d->input_max<2 || d->input_max>1024) return -3;
    uint8_t command[4]={d->command_register,d->command_register>>8,0,8}; /* SET_POWER ON */
    rc=t->xfer(t->ctx,command,4,0,0);if(rc) return rc;
    t->sleep_ms(t->ctx,10);
    command[3]=1; /* RESET; completion is a zero-length input packet */
    rc=t->xfer(t->ctx,command,4,0,0);if(rc) return rc;
    uint64_t start=t->clock_ms(t->ctx);
    uint8_t input[1024];
    for(;;) {
        t->sleep_ms(t->ctx,10);
        rc=t->xfer(t->ctx,0,0,input,d->input_max);
        if(!rc && !le16(input)) break;
        if(rc && rc!=-2) return rc; /* permit NACK while resetting */
        if(t->clock_ms(t->ctx)-start>=1000) return -1;
    }
    address[0]=d->report_register;address[1]=d->report_register>>8;
    return t->xfer(t->ctx,address,2,report,d->report_length);
}
int i2c_hid_read(struct i2c_hid_transport *t,const struct i2c_hid_descriptor *d,
    uint8_t *buffer,unsigned capacity) {
    if(capacity<d->input_max || d->input_max<2) return -3;
    int rc=t->xfer(t->ctx,0,0,buffer,d->input_max);if(rc) return rc;
    unsigned n=le16(buffer);
    if(!n) return -4;
    if(n==0xffff || n==2) return 0;
    if(n<2 || n>d->input_max) return -3;
    return n-2;
}
