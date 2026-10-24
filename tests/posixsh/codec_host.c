#define _POSIX_C_SOURCE 200809L
#include "ast_codec.h"
#include <mrsh/parser.h>
#include <mrsh/buffer.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cases.h"
static unsigned checks,mutations;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"codec failure line %d: %s\n",__LINE__,#x);abort();}}while(0)
static uint32_t random_state=0x713c904a;
static uint32_t random_number(void){random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
int main(void) {
 for(unsigned i=0;i<sizeof shell_cases/sizeof shell_cases[0];i++) {
  const struct shell_case *test=&shell_cases[i];
  struct mrsh_parser *p=mrsh_parser_with_data(test->script,strlen(test->script));
  struct mrsh_program *program=mrsh_parse_program(p);
  if(mrsh_parser_error(p,NULL)) {CHECK(test->status==2);mrsh_program_destroy(program);mrsh_parser_destroy(p);continue;}
  CHECK(program!=NULL);char *encoded=NULL;size_t size=0;
  CHECK(sh_ast_encode(&program->node,&encoded,&size));
  struct mrsh_node *copy=sh_ast_decode(encoded,size);CHECK(copy!=NULL);
  char *again=NULL;size_t again_size=0;CHECK(sh_ast_encode(copy,&again,&again_size));
  CHECK(size==again_size && !memcmp(encoded,again,size));free(again);mrsh_node_destroy(copy);
  for(size_t cut=0;cut<size;cut+=size/31+1)CHECK(sh_ast_decode(encoded,cut)==NULL);
  if(size)CHECK(sh_ast_decode(encoded,size-1)==NULL);
  char *mutation=malloc(size);CHECK(mutation!=NULL);
  for(unsigned n=0;n<256;n++) {
   mutations++;
   memcpy(mutation,encoded,size);size_t where=random_number()%size;
   mutation[where]^=(char)(1u<<(random_number()%8));
   struct mrsh_node *altered=sh_ast_decode(mutation,size);
   if(altered){char *result=NULL;size_t length=0;CHECK(sh_ast_encode(altered,&result,&length));free(result);mrsh_node_destroy(altered);}
  }
  free(mutation);free(encoded);mrsh_program_destroy(program);mrsh_parser_destroy(p);
 }
 uint32_t invalid[]={0x53484101,0xffffffff};CHECK(sh_ast_decode(invalid,sizeof invalid)==NULL);
 CHECK(sh_ast_decode(invalid,SH_CONTEXT_MAX+1)==NULL);
 struct mrsh_node unknown={.type=999};char *out=NULL;size_t n=0;CHECK(!sh_ast_encode(&unknown,&out,&n) && !out);
 /* Valid but oversized nesting is refused before a worker can be launched. */
 struct mrsh_word *word=&mrsh_word_string_create(strdup("deep"),true)->word;
 for(unsigned i=0;i<110;i++){struct mrsh_array a={0};CHECK(mrsh_array_add(&a,word)>=0);word=&mrsh_word_list_create(&a,false)->word;}
 CHECK(!sh_ast_encode(&word->node,&out,&n));mrsh_word_destroy(word);
 printf("PASS: %u typed AST codec checks, truncations, %u deterministic mutations and depth/size bounds\n",checks,mutations);
 return 0;
}
