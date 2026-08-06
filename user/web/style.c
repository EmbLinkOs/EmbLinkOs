/* user/web/style.c -- the user-agent stylesheet. See style.h. */
#include <string.h>
#include "style.h"

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return !*a && !*b;
}

void vstyle_root(struct vstyle *o) {
    memset(o, 0, sizeof *o);
    o->display = VD_BLOCK;
    o->size = 0;                       /* body */
}

void vstyle_for(const char *tag, const struct vstyle *p, struct vstyle *o) {
    /* INHERIT the text properties, RESET the box properties. That split is
     * CSS's inheritance model; without it a <b> inside an <h1> would fall back
     * to body size, and every nested element would re-indent. */
    memset(o, 0, sizeof *o);
    o->size = p->size; o->bold = p->bold; o->italic = p->italic;
    o->mono = p->mono; o->underline = p->underline; o->link = p->link;
    o->pre = p->pre;
    o->display = VD_INLINE;

    /* --- headings: size carries the hierarchy, weight reinforces it ---- */
    if (ieq(tag,"h1")) { o->display=VD_BLOCK; o->size=3; o->bold=1; o->margin_top=18; o->margin_bottom=10; }
    else if (ieq(tag,"h2")) { o->display=VD_BLOCK; o->size=2; o->bold=1; o->margin_top=16; o->margin_bottom=8; }
    else if (ieq(tag,"h3")) { o->display=VD_BLOCK; o->size=2; o->bold=1; o->margin_top=14; o->margin_bottom=6; }
    else if (ieq(tag,"h4") || ieq(tag,"h5") || ieq(tag,"h6"))
                            { o->display=VD_BLOCK; o->bold=1; o->margin_top=12; o->margin_bottom=5; }

    /* --- flow --- */
    else if (ieq(tag,"p"))  { o->display=VD_BLOCK; o->margin_top=0; o->margin_bottom=12; }
    else if (ieq(tag,"div") || ieq(tag,"section") || ieq(tag,"article") ||
             ieq(tag,"header") || ieq(tag,"footer") || ieq(tag,"nav") ||
             ieq(tag,"main") || ieq(tag,"body") || ieq(tag,"html") ||
             ieq(tag,"document") || ieq(tag,"form") || ieq(tag,"table") ||
             ieq(tag,"tr") || ieq(tag,"tbody") || ieq(tag,"thead"))
                            { o->display=VD_BLOCK; }
    else if (ieq(tag,"blockquote")) { o->display=VD_BLOCK; o->indent=20; o->margin_bottom=12; }
    else if (ieq(tag,"hr")) { o->display=VD_BLOCK; o->margin_top=10; o->margin_bottom=10; }

    /* --- lists: the marker is a BOX property, so it does not inherit into
     *     the item's own children (a <b> inside an <li> must not re-bullet) */
    else if (ieq(tag,"ul")) { o->display=VD_BLOCK; o->indent=22; o->margin_bottom=10; }
    else if (ieq(tag,"ol")) { o->display=VD_BLOCK; o->indent=22; o->margin_bottom=10; }
    else if (ieq(tag,"li")) { o->display=VD_LIST_ITEM; o->marker=VM_BULLET; o->margin_bottom=3; }
    else if (ieq(tag,"dt")) { o->display=VD_BLOCK; o->bold=1; }
    else if (ieq(tag,"dd")) { o->display=VD_BLOCK; o->indent=20; o->margin_bottom=6; }

    /* --- preformatted: the one place whitespace survives --- */
    else if (ieq(tag,"pre")) { o->display=VD_BLOCK; o->mono=1; o->pre=1;
                               o->margin_top=8; o->margin_bottom=12; o->indent=10; }

    /* --- inline --- */
    else if (ieq(tag,"b") || ieq(tag,"strong")) o->bold = 1;
    else if (ieq(tag,"i") || ieq(tag,"em"))     o->italic = 1;
    else if (ieq(tag,"code") || ieq(tag,"kbd") || ieq(tag,"samp") || ieq(tag,"tt")) o->mono = 1;
    else if (ieq(tag,"a"))  { o->link = 1; o->underline = 1; }
    else if (ieq(tag,"small")) o->size = 1;
    else if (ieq(tag,"td") || ieq(tag,"th")) { o->display = VD_BLOCK; if (ieq(tag,"th")) o->bold = 1; }

    /* --- never shown --- */
    else if (ieq(tag,"head") || ieq(tag,"title") || ieq(tag,"script") ||
             ieq(tag,"style") || ieq(tag,"meta") || ieq(tag,"link"))
        o->display = VD_NONE;
}
