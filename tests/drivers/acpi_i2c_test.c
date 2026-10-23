#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "acpi/serial_i2c.h"
static uint8_t bytes[256];
static unsigned fixture(void) {
    memset(bytes,0,sizeof bytes);
    const char source[]="\\_SB.PCI0.I2C0";
    unsigned len=15+sizeof source;
    bytes[0]=0x8e;bytes[1]=len;
    uint8_t *d=bytes+3;d[0]=1;d[2]=1;d[6]=1;d[7]=6;
    uint32_t speed=400000;memcpy(d+9,&speed,4);d[13]=0x15;
    memcpy(d+15,source,sizeof source);
    bytes[3+len]=0x79;return 3+len+2;
}
int main(void) {
    struct acpi_i2c_connection c;
    unsigned size=fixture();assert(!acpi_i2c_connection(bytes,size,&c));
    assert(c.address==0x15 && c.speed==400000 && !strcmp(c.source,"\\_SB.PCI0.I2C0"));
    for(unsigned n=0;n<size;n++)assert(acpi_i2c_connection(bytes,n,&c));
    bytes[size-3]='x';assert(acpi_i2c_connection(bytes,size,&c));
    fixture();bytes[6]=1;assert(acpi_i2c_connection(bytes,size,&c));
    fixture();bytes[7]=1;assert(acpi_i2c_connection(bytes,size,&c));
    fixture();bytes[16]=0x78;assert(acpi_i2c_connection(bytes,size,&c));
    fixture();bytes[10]=5;assert(acpi_i2c_connection(bytes,size,&c));
    fixture();memmove(bytes+size-2,bytes,size);assert(acpi_i2c_connection(bytes,2*size-2,&c));
    uint32_t seed=17;
    for(unsigned i=0;i<10000;i++) {
        unsigned n=i%sizeof bytes;
        for(unsigned j=0;j<n;j++){seed=seed*1664525+1013904223;bytes[j]=seed>>24;}
        (void)acpi_i2c_connection(bytes,n,&c);
    }
    puts("ACPI I2C resources: controller mode, address/speed, duplicate paths, truncation and 10k malformed templates OK");
}
