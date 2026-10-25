#ifndef EMBLINK_ARGV_H
#define EMBLINK_ARGV_H
#include <stddef.h>
#include "include/errno.h"
/* Bounded console argv decoding. Quotes group text, backslash quotes the next
 * byte outside single quotes. No variables, pipelines or command substitution.
 * argv has room for max_args pointers plus a terminating NULL. */
static inline int console_argv(const char *src,char *buf,size_t capacity,char **argv,unsigned max_args) {
    size_t written=0;unsigned count=0;
    argv[0]=NULL;
    while(*src) {
        while(*src==' ' || *src=='\t') src++;
        if(!*src) break;
        if(count==max_args || written>=capacity) return -EMBK_E2BIG;
        argv[count++]=buf+written;
        char quote=0;
        while(*src && (quote || (*src!=' ' && *src!='\t'))) {
            char c=*src++;
            if(c=='\\' && quote!='\'') {
                if(!*src) return -EMBK_EINVAL;
                c=*src++;
            } else if(c=='\'' || c=='"') {
                if(!quote) {quote=c;continue;}
                if(quote==c) {quote=0;continue;}
            }
            if(written+1>=capacity) return -EMBK_E2BIG;
            buf[written++]=c;
        }
        if(quote) return -EMBK_EINVAL;
        if(written>=capacity) return -EMBK_E2BIG;
        buf[written++]=0;
    }
    argv[count]=NULL;return count;
}
#endif
