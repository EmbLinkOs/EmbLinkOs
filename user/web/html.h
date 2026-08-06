/* user/web/html.h -- an HTML parser and document model.
 *
 * Scope, stated honestly because a browser is a thing people have opinions
 * about: this parses the STRUCTURE of a document -- elements, attributes,
 * text, entities -- into a tree. It does not run scripts, does not implement
 * CSS, and does not attempt the HTML5 spec's full error-recovery algorithm.
 * What it does implement is the part that makes real documents readable:
 * implicit end tags, void elements, attribute quoting, character references,
 * and skipping the contents of <script> and <style> wholesale.
 *
 * That subset is not a toy. It is what a documentation site, a README, a man
 * page, an RSS-era blog and everything our own httpd serves actually are.
 *
 * The tree is allocated from a caller-supplied arena. No malloc: a parser
 * that cannot run out of memory in a bounded way is a parser that can be run
 * on untrusted input from the network, which is exactly what this is for.
 */
#ifndef _EMBLINK_WEB_HTML_H_
#define _EMBLINK_WEB_HTML_H_

#include <stddef.h>

#define HTML_TAG_MAX   16
#define HTML_HREF_MAX 512

enum html_kind { HTML_ELEM = 0, HTML_TEXT };

struct html_node {
    unsigned char kind;
    char  tag[HTML_TAG_MAX];        /* lowercased; HTML_ELEM only          */
    char *text;                     /* into the arena; HTML_TEXT only      */
    char *href;                     /* <a href> / <img src>, or NULL       */
    int   first_child, next_sibling, parent;   /* indices, -1 = none       */
};

struct html_doc {
    struct html_node *nodes;
    int  n, cap;
    char *strs;                     /* string arena                        */
    size_t strn, strcap;
    int  root;
    int  truncated;                 /* ran out of arena: tree is partial   */
};

/* Parse `src` (len bytes) into `doc`. The caller owns both arenas; the parser
 * never allocates. Returns the root node index, or -1 if the arenas were too
 * small to hold even the root. Always leaves `doc` walkable. */
int html_parse(struct html_doc *doc, const char *src, size_t len,
               struct html_node *node_arena, int node_cap,
               char *str_arena, size_t str_cap);

/* Resolve `href` (which may be absolute, root-relative or relative) against
 * `base`, writing an absolute URL into out. Returns 0 on success. Link
 * following is most of what a browser IS, and it lives or dies on this. */
int html_resolve_url(const char *base, const char *href, char *out, size_t cap);

#endif /* _EMBLINK_WEB_HTML_H_ */
