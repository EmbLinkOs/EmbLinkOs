#ifndef EMBLINK_ACPI_SERIAL_I2C_H
#define EMBLINK_ACPI_SERIAL_I2C_H
#include <stdint.h>
/* ACPI SerialBus I2C descriptors. Source points into the caller-owned _CRS
 * buffer. Reject duplicate connections, truncated records and unterminated
 * source paths before any controller can be touched. */
struct acpi_i2c_connection {const char *source;uint32_t speed;uint16_t address;};
static inline int acpi_i2c_connection(const uint8_t *raw,unsigned size,struct acpi_i2c_connection *out) {
    unsigned count=0,ended=0;
    for(unsigned at=0;at<size;) {
        unsigned tag=raw[at++],len;
        if(tag&128) {
            if(size-at<2)return -1;
            len=raw[at]|((unsigned)raw[at+1]<<8);at+=2;
        } else len=tag&7;
        if(len>size-at)return -1;
        const uint8_t *d=raw+at;
        if(tag==0x8e) {
            if(len<9)return -1;
            if(d[2]==1) {
                if(++count>1 || len<16 || (d[0]!=1 && d[0]!=2) ||
                   (d[3]&1) || (d[4]&1))return -1;
                unsigned specific=d[7]|((unsigned)d[8]<<8);
                if(specific<6 || specific>=len-9)return -1;
                unsigned source_at=9+specific,source_len=len-source_at;
                if(source_len>256 || source_len<2 || d[len-1])return -1;
                for(unsigned i=source_at;i+1<len;i++)if(!d[i])return -1;
                unsigned address=d[13]|((unsigned)d[14]<<8);
                uint32_t speed=d[9]|((uint32_t)d[10]<<8)|((uint32_t)d[11]<<16)|((uint32_t)d[12]<<24);
                if(address<8 || address>0x77 || speed<100000 || speed>400000)return -1;
                *out=(struct acpi_i2c_connection){(const char *)d+source_at,speed,address};
            }
        } else if(!(tag&128) && (tag>>3)==15) {ended=len==1;break;}
        at+=len;
    }
    return ended && count==1?0:-1;
}
#endif
