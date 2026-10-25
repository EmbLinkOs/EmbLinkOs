#include <assert.h>
#include <stdio.h>
#include "session_policy.h"
int main(void){
    assert(embk_session_grant_ok("alex",0,"/home/alex",0));
    assert(embk_session_grant_ok("alex",0,"/home/alex/Documents",0));
    assert(embk_session_grant_ok("alex",0,"/system/bin",1));
    assert(embk_session_grant_ok("alex",0,"/run",0));
    const char *bad[]={"/","/etc","/home/alexander","/home/guest","/home/alex/../../etc","/system/./bin","/system//bin"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++)assert(!embk_session_grant_ok("alex",0,bad[i],0));
    assert(!embk_session_grant_ok("alex",0,"/system",0));
    assert(!embk_session_grant_ok("alex",0,"/data/apps",0));
    assert(embk_session_grant_ok("alex",1,"/data/apps",0));
    assert(!embk_session_grant_ok("alex",1,"/etc",0));
    puts("session profiles: native user/admin clamp, home boundaries, traversal and read-only system OK");
}
