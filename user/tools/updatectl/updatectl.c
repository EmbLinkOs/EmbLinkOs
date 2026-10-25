#include <stdio.h>
#include <string.h>
#include "embk.h"
static int status(void) {
    struct embk_update_info info;int rc=embk_update(EMBK_UPDATE_STATUS,&info,sizeof info);if(rc)return rc;
    printf("UPDATE phase=%u flags=%u job=%u release=%llu version=%s candidate=%llu progress=%u error=%d\n%s\n",
        info.phase,info.flags,info.job,(unsigned long long)info.release,info.current,
        (unsigned long long)info.candidate_release,info.progress,info.error,info.message);
    if(info.notes[0])printf("%s\n",info.notes);return 0;
}
int main(int argc,char **argv) {
    unsigned op=EMBK_UPDATE_STATUS;
    if(argc>1) {
        if(!strcmp(argv[1],"check"))op=EMBK_UPDATE_CHECK;
        else if(!strcmp(argv[1],"install"))op=EMBK_UPDATE_INSTALL;
        else if(!strcmp(argv[1],"rollback"))op=EMBK_UPDATE_ROLLBACK;
        else if(!strcmp(argv[1],"apps"))op=EMBK_UPDATE_APPLICATIONS;
        else if(strcmp(argv[1],"status")){fprintf(stderr,"usage: updatectl status|check|install|rollback|apps\n");return 2;}
    }
    int rc=op==EMBK_UPDATE_STATUS?status():embk_update(op,NULL,0);
    printf("UPDATE request %u result %d\n",op,rc);return rc?1:0;
}
