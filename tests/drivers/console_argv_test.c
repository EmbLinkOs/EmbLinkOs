#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "include/argv.h"
int main(void) {
    char buf[128];char *args[5];
    assert(console_argv("  /data/apps/settings/settings.elf --wifi\t",buf,sizeof buf,args,4)==2);
    assert(!strcmp(args[0],"/data/apps/settings/settings.elf") && !strcmp(args[1],"--wifi") && !args[2]);
    assert(console_argv("'path with spaces' \"a\\\"b\" '' x\\ y",buf,sizeof buf,args,4)==4);
    assert(!strcmp(args[0],"path with spaces") && !strcmp(args[1],"a\"b") && !args[2][0] && !strcmp(args[3],"x y") && !args[4]);
    assert(console_argv("x 'unfinished",buf,sizeof buf,args,4)==-22);
    assert(console_argv("x \\",buf,sizeof buf,args,4)==-22);
    assert(console_argv("a b c d e",buf,sizeof buf,args,4)==-7);
    assert(console_argv("abcd",buf,4,args,4)==-7);
    assert(console_argv("abc",buf,4,args,4)==1 && !strcmp(args[0],"abc"));
    assert(console_argv("",buf,0,args,4)==0 && !args[0]);
    assert(console_argv("x",buf,0,args,4)==-7);
    assert(console_argv("x '$HOME;$(id)'",buf,sizeof buf,args,4)==2 && !strcmp(args[1],"$HOME;$(id)"));
    puts("console argv: grouping, literal escapes, empty arguments and bounds OK");
}
