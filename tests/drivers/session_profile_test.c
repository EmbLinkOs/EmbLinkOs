#include <assert.h>
#include <stdio.h>
#define _start emblink_init_test_entry
#include "../../user/bin/init.c"
#undef _start
int main(void){
    const char *home="/home/alex";
    assert(profile_path_allowed(home,EMBK_NS_RW,home));
    assert(profile_path_allowed("/home/alex/Documents",EMBK_NS_RW,home));
    assert(profile_path_allowed("/system/bin",EMBK_NS_RO,home));
    assert(profile_path_allowed("/run",EMBK_NS_RW,home));
    assert(!profile_path_allowed("/",EMBK_NS_RW,home));
    assert(!profile_path_allowed("/etc",EMBK_NS_RW,home));
    assert(!profile_path_allowed("/system",EMBK_NS_RW,home));
    assert(!profile_path_allowed("/data/apps",EMBK_NS_RW,home));
    assert(!profile_path_allowed("/home/alexander",EMBK_NS_RW,home));
    assert(!profile_path_allowed("/home/.live-guest",EMBK_NS_RW,home));
    assert(!profile_path_allowed("/home/alex/../../etc",EMBK_NS_RW,home));
    assert(!profile_path_allowed("/system/./bin",EMBK_NS_RO,home));
    assert(!profile_path_allowed("/system//bin",EMBK_NS_RO,home));
    puts("session profiles: home/component boundaries, traversal, read-only system and isolated live home OK");
}
