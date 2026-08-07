/* user/web/style.c -- the user-agent stylesheet. See style.h. */
#include <string.h>
#include "style.h"
#include "html.h"
#include "css.h"

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
    o->color = p->color;          /* colour inherits, like every text property */
    /* text-align and line-height inherit too -- `body { text-align: center }`
     * must reach the paragraphs, which is the whole reason a page writes it
     * there. Background and border deliberately do NOT: they are box
     * properties, and inheriting them would paint every descendant. */
    o->align = p->align;
    o->line_height = p->line_height;
    o->display = VD_INLINE;

    /* --- headings: size carries the hierarchy, weight reinforces it ---- */
    if (ieq(tag,"h1")) { o->display=VD_BLOCK; o->size=3; o->bold=1; o->margin_top=18; o->margin_bottom=10; }
    else if (ieq(tag,"h2")) { o->display=VD_BLOCK; o->size=2; o->bold=1; o->margin_top=16; o->margin_bottom=8; }
    else if (ieq(tag,"h3")) { o->display=VD_BLOCK; o->size=2; o->bold=1; o->margin_top=14; o->margin_bottom=6; }
    else if (ieq(tag,"h4") || ieq(tag,"h5") || ieq(tag,"h6"))
                            { o->display=VD_BLOCK; o->bold=1; o->margin_top=12; o->margin_bottom=5; }

    /* <img> is inline-level, like text: a picture in a sentence sits IN the
     * sentence. render.c gives it a box; the stylist only says what kind of
     * box it is. */
    else if (ieq(tag,"img")) { o->display = VD_IMAGE; }

    /* --- form controls. `type` decides which box an <input> is, which is the
     * one place a tag's ATTRIBUTE and not its name selects the display -- and
     * exactly why the stylist rather than the renderer decides it. --- */
    else if (ieq(tag,"input")) {
        const char *ty = 0;   /* filled by the caller via vstyle_for_node */
        (void)ty;
        o->display = VD_FIELD;   /* refined to VD_BUTTON below when type says so */
    }
    else if (ieq(tag,"textarea")) { o->display = VD_FIELD; }
    else if (ieq(tag,"button"))   { o->display = VD_BUTTON; }
    else if (ieq(tag,"form"))     { o->display = VD_BLOCK; o->margin_top=8; o->margin_bottom=10; }
    else if (ieq(tag,"label"))    { o->display = VD_INLINE; }

    /* --- tables. A data table is not "tables as layout" (the practice
     * docs/BROWSER.md rightly refuses); it is how a reference page states a
     * grid of facts, and a documentation browser that cannot show one is
     * missing the format its own subject matter is written in. --- */
    else if (ieq(tag,"table")) { o->display=VD_TABLE; o->margin_top=10; o->margin_bottom=14; }
    else if (ieq(tag,"tr"))    { o->display=VD_ROW; }
    else if (ieq(tag,"th"))    { o->display=VD_CELL; o->bold=1; }
    else if (ieq(tag,"td"))    { o->display=VD_CELL; }
    else if (ieq(tag,"caption")) { o->display=VD_CAPTION; o->size=1; o->margin_bottom=4; }
    /* row GROUPS are transparent: they carry no box of their own, and the
     * table walker descends through them to find the rows. */
    else if (ieq(tag,"thead") || ieq(tag,"tbody") || ieq(tag,"tfoot") ||
             ieq(tag,"colgroup") || ieq(tag,"col")) { o->display=VD_BLOCK; }

    /* --- flow --- */
    else if (ieq(tag,"p"))  { o->display=VD_BLOCK; o->margin_top=0; o->margin_bottom=12; }
    else if (ieq(tag,"figure")) { o->display=VD_BLOCK; o->margin_top=10; o->margin_bottom=12; }
    else if (ieq(tag,"figcaption")) { o->display=VD_BLOCK; o->size=1; }
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

/* The full stylist: UA sheet -> author cascade -> inline style. Origin order
 * is the cascade, and it is the whole reason these are three calls and not one
 * -- each stage may override the last, and none of them may see the others'
 * inputs. Everything downstream still reads only `struct vstyle`, which is the
 * seam docs/BROWSER.md §4 promised would make CSS additive. */
void vstyle_for_node(struct html_doc *doc, int node, const struct vstyle *parent,
                     const struct css_sheet *sheet, struct vstyle *out) {
    const char *tag = (doc && node >= 0 && node < doc->n) ? doc->nodes[node].tag : "";
    vstyle_for(tag, parent, out);
    /* An <input> is a FIELD or a BUTTON depending on its type -- the one case
     * where an attribute picks the box. Done here, where the node is in hand,
     * so vstyle_for stays a pure function of the tag. */
    if (out->display == VD_FIELD && doc && node >= 0 && node < doc->n) {
        const char *ty = doc->nodes[node].type;
        if (ty && (!strcmp(ty, "submit") || !strcmp(ty, "button") ||
                   !strcmp(ty, "reset")))
            out->display = VD_BUTTON;
        else if (ty && !strcmp(ty, "hidden"))
            out->display = VD_NONE;
    }
    if (sheet && doc && node >= 0) css_sheet_apply(sheet, doc, node, out);
    if (doc && node >= 0 && node < doc->n && doc->nodes[node].style)
        css_apply_decls(doc->nodes[node].style, strlen(doc->nodes[node].style), out);
}
