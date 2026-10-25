/* Typed AST transport for explicit-spawn shell workers. No textual re-expansion
 * and no pointers from one address space are trusted in another. */
#include "ast_codec.h"
#include <mrsh/buffer.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define MAX_NODES 16384u
#define MAX_DEPTH 96u
#define BIT(n) (1u<<(n))
enum { NIL,PROGRAM,LIST,PIPELINE,BINOP,SIMPLE,BRACE,SUBSHELL,IF,FOR,LOOP,CASE,FUNCTION,
       STRING,PARAMETER,COMMAND_WORD,ARITHMETIC,WORD_LIST,REDIRECT,ASSIGNMENT,CASE_ITEM,SHAPES };
#define WORDS (BIT(STRING)|BIT(PARAMETER)|BIT(COMMAND_WORD)|BIT(ARITHMETIC)|BIT(WORD_LIST))
#define COMMANDS (BIT(SIMPLE)|BIT(BRACE)|BIT(SUBSHELL)|BIT(IF)|BIT(FOR)|BIT(LOOP)|BIT(CASE)|BIT(FUNCTION))
#define AND_OR (BIT(PIPELINE)|BIT(BINOP))
enum field_kind { END,TEXT,NUMBER,BOOLEAN,CHILD,ARRAY };
struct field {enum field_kind kind;size_t offset;uint32_t mask,max;};
struct shape {size_t size;struct field fields[8];};
#define F(t,m,k,mask,max) {k,offsetof(struct t,m),mask,max}
#define T(t,m) F(t,m,TEXT,0,0)
#define N(t,m,max) F(t,m,NUMBER,0,max)
#define B(t,m) F(t,m,BOOLEAN,0,1)
#define C(t,m,mask) F(t,m,CHILD,mask,0)
#define A(t,m,mask) F(t,m,ARRAY,mask,0)
#define S(t,...) {sizeof(struct t),{__VA_ARGS__,{0}}}
static const struct shape shapes[SHAPES]={
 [PROGRAM]=S(mrsh_program,A(mrsh_program,body,BIT(LIST))),
 [LIST]=S(mrsh_command_list,C(mrsh_command_list,and_or_list,AND_OR),B(mrsh_command_list,ampersand)),
 [PIPELINE]=S(mrsh_pipeline,A(mrsh_pipeline,commands,COMMANDS),B(mrsh_pipeline,bang)),
 [BINOP]=S(mrsh_binop,N(mrsh_binop,type,MRSH_BINOP_OR),C(mrsh_binop,left,AND_OR),C(mrsh_binop,right,AND_OR)),
 [SIMPLE]=S(mrsh_simple_command,C(mrsh_simple_command,name,WORDS|BIT(NIL)),A(mrsh_simple_command,arguments,WORDS),A(mrsh_simple_command,io_redirects,BIT(REDIRECT)),A(mrsh_simple_command,assignments,BIT(ASSIGNMENT))),
 [BRACE]=S(mrsh_brace_group,A(mrsh_brace_group,body,BIT(LIST))),
 [SUBSHELL]=S(mrsh_subshell,A(mrsh_subshell,body,BIT(LIST))),
 [IF]=S(mrsh_if_clause,A(mrsh_if_clause,condition,BIT(LIST)),A(mrsh_if_clause,body,BIT(LIST)),C(mrsh_if_clause,else_part,COMMANDS|BIT(NIL))),
 [FOR]=S(mrsh_for_clause,T(mrsh_for_clause,name),B(mrsh_for_clause,in),A(mrsh_for_clause,word_list,WORDS),A(mrsh_for_clause,body,BIT(LIST))),
 [LOOP]=S(mrsh_loop_clause,N(mrsh_loop_clause,type,MRSH_LOOP_UNTIL),A(mrsh_loop_clause,condition,BIT(LIST)),A(mrsh_loop_clause,body,BIT(LIST))),
 [CASE]=S(mrsh_case_clause,C(mrsh_case_clause,word,WORDS),A(mrsh_case_clause,items,BIT(CASE_ITEM))),
 [FUNCTION]=S(mrsh_function_definition,T(mrsh_function_definition,name),C(mrsh_function_definition,body,COMMANDS),A(mrsh_function_definition,io_redirects,BIT(REDIRECT))),
 [STRING]=S(mrsh_word_string,T(mrsh_word_string,str),B(mrsh_word_string,single_quoted),B(mrsh_word_string,split_fields)),
 [PARAMETER]=S(mrsh_word_parameter,T(mrsh_word_parameter,name),N(mrsh_word_parameter,op,MRSH_PARAM_DHASH),B(mrsh_word_parameter,colon),C(mrsh_word_parameter,arg,WORDS|BIT(NIL))),
 [COMMAND_WORD]=S(mrsh_word_command,C(mrsh_word_command,program,BIT(PROGRAM)|BIT(NIL)),B(mrsh_word_command,back_quoted)),
 [ARITHMETIC]=S(mrsh_word_arithmetic,C(mrsh_word_arithmetic,body,WORDS)),
 [WORD_LIST]=S(mrsh_word_list,A(mrsh_word_list,children,WORDS),B(mrsh_word_list,double_quoted)),
 [REDIRECT]=S(mrsh_io_redirect,N(mrsh_io_redirect,io_number,UINT32_MAX),N(mrsh_io_redirect,op,MRSH_IO_DLESSDASH),C(mrsh_io_redirect,name,WORDS),A(mrsh_io_redirect,here_document,WORDS)),
 [ASSIGNMENT]=S(mrsh_assignment,T(mrsh_assignment,name),C(mrsh_assignment,value,WORDS)),
 [CASE_ITEM]=S(mrsh_case_item,A(mrsh_case_item,patterns,WORDS),A(mrsh_case_item,body,BIT(LIST))),
};
static unsigned identify(const void *ptr,uint32_t mask) {
 if(!ptr)return NIL;
 for(unsigned id=REDIRECT;id<SHAPES;id++)if(mask==BIT(id))return id;
 const struct mrsh_node *n=ptr;
 switch(n->type) {
 case MRSH_NODE_PROGRAM:return PROGRAM;
 case MRSH_NODE_COMMAND_LIST:return LIST;
 case MRSH_NODE_AND_OR_LIST:
  switch(((const struct mrsh_and_or_list *)ptr)->type) {
  case MRSH_AND_OR_LIST_PIPELINE:return PIPELINE;case MRSH_AND_OR_LIST_BINOP:return BINOP;
  }break;
 case MRSH_NODE_COMMAND:{unsigned t=((const struct mrsh_command *)ptr)->type;return t<=MRSH_FUNCTION_DEFINITION?SIMPLE+t:SHAPES;}
 case MRSH_NODE_WORD:{unsigned t=((const struct mrsh_word *)ptr)->type;return t<=MRSH_WORD_LIST?STRING+t:SHAPES;}
 }
 return SHAPES;
}
static void initialize(void *ptr,unsigned id) {
 struct mrsh_node *n=ptr;
 if(id==PROGRAM)n->type=MRSH_NODE_PROGRAM;
 else if(id==LIST)n->type=MRSH_NODE_COMMAND_LIST;
 else if(id==PIPELINE || id==BINOP) {
  n->type=MRSH_NODE_AND_OR_LIST;
  ((struct mrsh_and_or_list *)ptr)->type=id==PIPELINE?MRSH_AND_OR_LIST_PIPELINE:MRSH_AND_OR_LIST_BINOP;
 } else if(id>=SIMPLE && id<=FUNCTION) {
  n->type=MRSH_NODE_COMMAND;((struct mrsh_command *)ptr)->type=id-SIMPLE;
 } else if(id>=STRING && id<=WORD_LIST) {
  n->type=MRSH_NODE_WORD;((struct mrsh_word *)ptr)->type=id-STRING;
 }
}
static void release(void *ptr,unsigned id) {
 if(!ptr)return;
 for(const struct field *f=shapes[id].fields;f->kind;f++) {
  void *slot=(char *)ptr+f->offset;
  if(f->kind==TEXT)free(*(char **)slot);
  else if(f->kind==CHILD){void *p=*(void **)slot;release(p,identify(p,f->mask));}
  else if(f->kind==ARRAY) {
   struct mrsh_array *a=slot;
   for(size_t i=0;i<a->len;i++)release(a->data[i],identify(a->data[i],f->mask));
   free(a->data);
  }
 }
 free(ptr);
}
struct writer {struct mrsh_buffer buffer;unsigned nodes;bool ok;};
static void append(struct writer *w,const void *p,size_t n) {
 if(!w->ok)return;
 if(n>SH_CONTEXT_MAX-w->buffer.len || !mrsh_buffer_append(&w->buffer,p,n))w->ok=false;
}
static void put(struct writer *w,uint32_t n){append(w,&n,sizeof n);}
static void encode(struct writer *w,const void *ptr,uint32_t mask,unsigned depth) {
 if(!w->ok)return;
 unsigned id=identify(ptr,mask);
 if(id>=SHAPES || !(mask&BIT(id)) || depth>MAX_DEPTH || ++w->nodes>MAX_NODES){w->ok=false;return;}
 put(w,id);if(!ptr)return;
 for(const struct field *f=shapes[id].fields;f->kind && w->ok;f++) {
  const void *slot=(const char *)ptr+f->offset;
  switch(f->kind) {
  case TEXT:{const char *s=*(char *const *)slot;if(!s){w->ok=false;break;}size_t n=strlen(s);if(n>SH_CONTEXT_MAX){w->ok=false;break;}put(w,n);append(w,s,n);break;}
  case NUMBER:{uint32_t n;memcpy(&n,slot,4);if(n>f->max)w->ok=false;put(w,n);break;}
  case BOOLEAN:put(w,*(const bool *)slot?1:0);break;
  case CHILD:encode(w,*(void *const *)slot,f->mask,depth+1);break;
  case ARRAY:{const struct mrsh_array *a=slot;if(a->len>MAX_NODES){w->ok=false;break;}put(w,a->len);for(size_t i=0;i<a->len && w->ok;i++)encode(w,a->data[i],f->mask,depth+1);break;}
  default:break;
  }
 }
}
struct reader {const unsigned char *p;size_t left;unsigned nodes;bool ok;};
static uint32_t get(struct reader *r) {
 uint32_t n=0;if(!r->ok)return 0;
 if(r->left<4){r->ok=false;return 0;}
 memcpy(&n,r->p,4);r->p+=4;r->left-=4;return n;
}
static void *decode(struct reader *r,uint32_t mask,unsigned depth) {
 if(!r->ok)return NULL;
 unsigned id=get(r);
 if(!r->ok || id>=SHAPES || !(mask&BIT(id)) || depth>MAX_DEPTH || ++r->nodes>MAX_NODES){r->ok=false;return NULL;}
 if(!id)return NULL;
 void *ptr=calloc(1,shapes[id].size);if(!ptr){r->ok=false;return NULL;}initialize(ptr,id);
 for(const struct field *f=shapes[id].fields;f->kind && r->ok;f++) {
  void *slot=(char *)ptr+f->offset;
  switch(f->kind) {
  case TEXT:{uint32_t n=get(r);if(!r->ok || n>r->left || memchr(r->p,0,n)){r->ok=false;break;}
   char *s=malloc((size_t)n+1);if(!s){r->ok=false;break;}memcpy(s,r->p,n);s[n]=0;*(char **)slot=s;r->p+=n;r->left-=n;break;}
  case NUMBER:{uint32_t n=get(r);if(n>f->max)r->ok=false;else memcpy(slot,&n,4);break;}
  case BOOLEAN:{uint32_t n=get(r);if(n>1)r->ok=false;else *(bool *)slot=n!=0;break;}
  case CHILD:*(void **)slot=decode(r,f->mask,depth+1);break;
  case ARRAY:{uint32_t n=get(r);struct mrsh_array *a=slot;
   if(!r->ok || n>MAX_NODES-r->nodes || n>r->left/4){r->ok=false;break;}
   if(n){a->data=calloc(n,sizeof *a->data);if(!a->data){r->ok=false;break;}a->cap=n;}
   for(unsigned i=0;i<n && r->ok;i++){void *v=decode(r,f->mask,depth+1);if(r->ok)a->data[a->len++]=v;}
   break;}
  default:break;
  }
 }
 if(!r->ok){release(ptr,id);return NULL;}return ptr;
}
bool sh_ast_encode(const struct mrsh_node *node,char **out,size_t *size) {
 struct writer w={.ok=true};put(&w,0x53484101);
 encode(&w,node,BIT(PROGRAM)|BIT(LIST)|AND_OR|COMMANDS|WORDS,0);
 if(!w.ok){mrsh_buffer_finish(&w.buffer);errno=E2BIG;return false;}
 *size=w.buffer.len;*out=mrsh_buffer_steal(&w.buffer);return true;
}
struct mrsh_node *sh_ast_decode(const void *data,size_t size) {
 if(!data || size>SH_CONTEXT_MAX){errno=EINVAL;return NULL;}
 struct reader r={.p=data,.left=size,.ok=true};
 if(get(&r)!=0x53484101){errno=EINVAL;return NULL;}
 void *node=decode(&r,BIT(PROGRAM)|BIT(LIST)|AND_OR|COMMANDS|WORDS,0);
 if(!r.ok || r.left){if(node)release(node,identify(node,0));errno=EINVAL;return NULL;}
 return node;
}
