#ifndef EMBLINK_MRSH_AST_CODEC_H
#define EMBLINK_MRSH_AST_CODEC_H
#include <mrsh/ast.h>
#include <stddef.h>
#include <stdbool.h>
#define SH_CONTEXT_MAX (1024u*1024u)
/* Private, versioned worker transport, independent of pointers and padding. */
bool sh_ast_encode(const struct mrsh_node *,char **,size_t *);
struct mrsh_node *sh_ast_decode(const void *,size_t);
#endif
