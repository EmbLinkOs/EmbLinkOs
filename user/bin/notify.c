#include <stdio.h>
#include "embk.h"
int main(int argc,char **argv) {
    if(argc!=3) {fprintf(stderr,"usage: notify.elf <title> <message>\n");return 2;}
    int rc=embk_notify_send(0,argv[1],argv[2]);
    if(rc<0) {fprintf(stderr,"notify: could not deliver notification (%d)\n",rc);return 1;}
    return 0;
}
