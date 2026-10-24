#include "doc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}count++;}while(0)
static int count;
static long good(const char *p,char *b,size_t n){(void)p;(void)n;strcpy(b,"original");return 8;}
static long fail(const char *p,char *b,size_t n){(void)p;(void)b;(void)n;return -1;}
static long oversized(const char *p,char *b,size_t n){(void)p;(void)b;return (long)n+200;}
int main(void) {
 doc_init();CHECK(doc_count()==1);
 CHECK(doc_open("/unreadable",fail)==-2 && doc_count()==1 && !doc_cur()->path[0]);
 CHECK(doc_open("/oversized",oversized)==-2 && doc_count()==1);
 int i=doc_open("/report.txt",good);CHECK(i>=0 && doc_count()==1 && !strcmp(doc_cur()->text,"original"));
 CHECK(doc_open("/report.txt",fail)==i && doc_count()==1);
 strcpy(doc_cur()->text,"modified");CHECK(doc_dirty(i));
 CHECK(doc_new()>=0);CHECK(doc_dirty(i));doc_select(i);CHECK(!strcmp(doc_cur()->text,"modified"));
 for(int k=1;k<DOC_MAX;k++){char path[40];snprintf(path,sizeof path,"/document-%d",k);CHECK(doc_open(path,good)>=0);}
 CHECK(doc_count()==DOC_MAX);CHECK(doc_open("/full",good)==-1);
 doc_select(i);CHECK(doc_dirty(i));doc_mark_saved(i);CHECK(!doc_dirty(i));
 printf("doc-model-test: %d assertions passed\n",count);return 0;
}
