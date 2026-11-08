/* vt_grid.c -- cells, styles, the two screens and the scrollback. See vt.h.
 *
 * The parser (vt_parse.c) decides WHAT a byte means; this file is where it
 * lands: characters printed at the cursor, rows scrolled (into the scrollback
 * when the whole primary screen moves), cells erased with the pen's
 * background, and a resize that reflows the primary screen and re-wraps the
 * scrollback, which is kept as logical lines for exactly that. */
#include "vt_internal.h"
#include "../../shell/edit/wcwidth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct vt_cell plain_blank={' ',0,1,0};

/* --- colours ----------------------------------------------------------------- */

uint32_t vt_default_color(unsigned i) {
    static const uint32_t colors[16]={0x30343b,0xcf666c,0x73bb85,0xdbbf70,0x789fdb,0xb98bcc,0x6abec5,0xd9dfe7,
        0x81858e,0xf58b91,0x91dba0,0xf0d988,0x99bdf4,0xd4aaf1,0x8bdde2,0xf5f7fa};
    if (i<16) return colors[i];
    if (i<232) { i-=16; unsigned r=i/36,g=(i/6)%6,b=i%6; return ((r?55+r*40:0)<<16)|((g?55+g*40:0)<<8)|(b?55+b*40:0); }
    unsigned g=8+(i<256?i-232:23)*10; return (g<<16)|(g<<8)|g;
}
uint32_t vt_rgb(const struct vt *v,uint32_t c,bool fg) {
    switch (VT_COLOR_KIND(c)) {
    case 1: {unsigned i=c&0xff;return (v->over_set[i>>3]>>(i&7))&1?v->over.color[i]:v->base.color[i];}
    case 2: return c&0xffffff;
    default: return fg?(v->over_fg?v->over.fg:v->base.fg):(v->over_bg?v->over.bg:v->base.bg);
    }
}
uint32_t vt_cursor_rgb(const struct vt *v) { return v->over_cursor?v->over.cursor:v->base.cursor; }
void vt_set_palette(struct vt *v,const struct vt_palette *p) { v->base=*p;v->revision++; }

/* The one width table, shared with the shell's line editor (wcwidth.h): the
 * editor counts cells to place its cursor, so the two must never disagree. */
int vt_width(uint32_t c) { return wcwidth_cp(c); }
unsigned vt_encode(uint32_t c,char out[4]) {
    if (c<0x80) {out[0]=(char)c;return 1;}
    if (c<0x800) {out[0]=(char)(0xc0|(c>>6));out[1]=(char)(0x80|(c&63));return 2;}
    if (c<0x10000) {out[0]=(char)(0xe0|(c>>12));out[1]=(char)(0x80|((c>>6)&63));out[2]=(char)(0x80|(c&63));return 3;}
    out[0]=(char)(0xf0|(c>>18));out[1]=(char)(0x80|((c>>12)&63));out[2]=(char)(0x80|((c>>6)&63));out[3]=(char)(0x80|(c&63));return 4;
}

/* --- the scrollback, first: compaction may have to shorten it ----------------- */

static uint32_t line_rows(const struct vt_cell *cells,uint32_t n,int cols) {
    uint32_t rows=1;int x=0;
    for (uint32_t i=0;i<n;i++) {
        int w=cells[i].width;
        if (!w) continue;
        if (x+w>cols) {rows++;x=0;}
        x+=w;
    }
    return rows;
}
static struct vt_hline *history_line(const struct vt *v,uint32_t i) {   /* 0 = the oldest */
    return &v->history[(v->history_head+i)%v->history_cap];
}
static size_t line_bytes(const struct vt_hline *l) { return l->n*sizeof *l->cells+sizeof *l; }
static void history_drop_oldest(struct vt *v) {
    struct vt_hline *l=history_line(v,0);
    v->history_bytes-=line_bytes(l);v->history_rows-=l->rows;
    free(l->cells);l->cells=NULL;
    v->history_head=(v->history_head+1)%v->history_cap;
    v->history_lines--;
}
static void history_drop_newest(struct vt *v) {
    struct vt_hline *l=history_line(v,v->history_lines-1);
    v->history_bytes-=line_bytes(l);v->history_rows-=l->rows;
    free(l->cells);l->cells=NULL;
    v->history_lines--;
}
static void history_trim(struct vt *v) {
    while (v->history_lines && v->history_bytes>v->history_limit) history_drop_oldest(v);
}
static bool history_grow(struct vt *v) {
    uint32_t cap=v->history_cap?v->history_cap*2:256;
    struct vt_hline *g=malloc(cap*sizeof *g);
    if (!g) return false;
    for (uint32_t i=0;i<v->history_lines;i++) g[i]=*history_line(v,i);
    free(v->history);v->history=g;v->history_cap=cap;v->history_head=0;
    return true;
}
static bool cell_blank(const struct vt_cell *c) { return c->cp==' ' && !c->style && c->width==1 && !c->flags; }
/* The cells of a row that belong to its line: all of a wrapped row but a
 * padding cell, up to the last written one of any other. */
static uint32_t row_length(const struct vt_row *r,int cols) {
    uint32_t n=(uint32_t)cols;
    if (r->wrapped) return r->pad?n-1:n;
    while (n && cell_blank(&r->cells[n-1])) n--;
    return n;
}
/* A row leaves the top of the primary screen. A row that wrapped is kept
 * whole and the next one continues its line. */
static void history_push(struct vt *v,const struct vt_row *r,int cols) {
    uint32_t n=row_length(r,cols);
    struct vt_hline *last=v->history_lines?history_line(v,v->history_lines-1):NULL;
    if (last && last->open) {
        struct vt_cell *g=realloc(last->cells,(last->n+n?last->n+n:1)*sizeof *g);
        if (g) {
            memcpy(g+last->n,r->cells,n*sizeof *g);last->cells=g;last->n+=n;
            v->history_bytes+=n*sizeof *g;
            v->history_rows-=last->rows;last->rows=line_rows(last->cells,last->n,cols);v->history_rows+=last->rows;
        }
        last->open=r->wrapped;
    } else {
        if (v->history_lines==v->history_cap && !history_grow(v)) return;
        struct vt_hline *l=&v->history[(v->history_head+v->history_lines)%v->history_cap];
        l->cells=malloc((n?n:1)*sizeof *l->cells);
        if (!l->cells) return;
        memcpy(l->cells,r->cells,n*sizeof *l->cells);
        l->n=n;l->open=r->wrapped;l->rows=line_rows(l->cells,n,cols);
        v->history_lines++;v->history_rows+=l->rows;v->history_bytes+=line_bytes(l);
    }
    v->scrolls++;
    history_trim(v);
}
void vt_clear_history(struct vt *v) {
    while (v->history_lines) history_drop_oldest(v);
    v->history_bytes=0;v->history_rows=0;v->revision++;
}
void vt_set_history_limit(struct vt *v,size_t bytes) { v->history_limit=bytes;history_trim(v);v->revision++; }

/* --- interned styles, clusters and hyperlinks ----------------------------------
 * Every distinct look gets an id; cells carry the id. When a table fills, the
 * ids no cell uses any more are dropped and the rest renumbered. If that frees
 * too little -- a scrollback full of truecolour images can really use 65535
 * looks -- the oldest half of the scrollback goes, so a full table costs one
 * pass per many thousand new looks, never one per character. */

static uint32_t style_hash(const struct vt_style *s) {
    const unsigned char *b=(const unsigned char *)s;uint32_t h=2166136261u;
    for (size_t i=0;i<sizeof *s;i++) h=(h^b[i])*16777619u;
    return h;
}
static void style_rehash(struct vt *v,uint32_t cap) {
    uint32_t *t=malloc(cap*sizeof *t);
    if (!t) return;
    memset(t,0xff,cap*sizeof *t);
    for (uint32_t i=0;i<v->nstyles;i++) {
        uint32_t h=style_hash(&v->styles[i])&(cap-1);
        while (t[h]!=UINT32_MAX) h=(h+1)&(cap-1);
        t[h]=i;
    }
    free(v->style_hash);v->style_hash=t;v->style_hash_cap=cap;
}

struct marks { uint8_t *styles, *clusters; };
struct remap { uint32_t *styles, *clusters; };
static void each_row_cells(struct vt *v,void (*fn)(struct vt *,struct vt_cell *,uint32_t,void *),void *ctx) {
    struct vt_screen *screens[2]={&v->primary,&v->alternate};
    for (int i=0;i<2;i++) for (int y=0;y<v->rows;y++) fn(v,screens[i]->rows[y]->cells,(uint32_t)v->cols,ctx);
    for (uint32_t i=0;i<v->history_lines;i++) {
        struct vt_hline *l=history_line(v,i);
        fn(v,l->cells,l->n,ctx);
    }
}
static void mark_cells(struct vt *v,struct vt_cell *c,uint32_t n,void *ctx) {
    struct marks *m=ctx;
    for (uint32_t i=0;i<n;i++) {
        if (c[i].style<v->nstyles) m->styles[c[i].style]=1;
        if (c[i].flags&VT_CELL_CLUSTER && c[i].cp<v->nclusters) m->clusters[c[i].cp]=1;
    }
}
static void remap_cells(struct vt *v,struct vt_cell *c,uint32_t n,void *ctx) {
    struct remap *r=ctx;
    for (uint32_t i=0;i<n;i++) {
        c[i].style=(uint16_t)(c[i].style<v->nstyles?r->styles[c[i].style]:0);
        if (c[i].flags&VT_CELL_CLUSTER) {
            if (c[i].cp<v->nclusters) c[i].cp=r->clusters[c[i].cp];
            else {c[i].cp=0xfffd;c[i].flags&=(uint8_t)~VT_CELL_CLUSTER;}
        }
    }
}
static void compact_once(struct vt *v) {
    uint32_t nc=v->nclusters?v->nclusters:1,old_links=v->nlinks;
    struct marks m={calloc(v->nstyles,1),calloc(nc,1)};
    struct remap r={calloc(v->nstyles,sizeof(uint32_t)),calloc(nc,sizeof(uint32_t))};
    uint8_t *used=calloc(old_links+1,1);uint32_t *link_map=calloc(old_links+1,sizeof(uint32_t));
    if (!m.styles || !m.clusters || !r.styles || !r.clusters || !used || !link_map) goto out;
    m.styles[0]=1;
    each_row_cells(v,mark_cells,&m);
    uint32_t ns=0,keep=0;
    for (uint32_t i=0;i<v->nstyles;i++) if (m.styles[i]) {r.styles[i]=ns;v->styles[ns++]=v->styles[i];}
    for (uint32_t i=0;i<v->nclusters;i++) if (m.clusters[i]) {
        r.clusters[i]=keep;
        memmove(v->clusters[keep],v->clusters[i],sizeof v->clusters[i]);v->cluster_len[keep]=v->cluster_len[i];
        keep++;
    }
    each_row_cells(v,remap_cells,&r);
    v->nstyles=ns;v->nclusters=keep;
    /* links: the ones a kept style or a pen still names */
    uint32_t *pens[4]={&v->primary.cursor.pen.link,&v->primary.saved.pen.link,
                       &v->alternate.cursor.pen.link,&v->alternate.saved.pen.link};
    for (uint32_t i=0;i<v->nstyles;i++) if (v->styles[i].link<=old_links) used[v->styles[i].link]=1;
    for (int i=0;i<4;i++) if (*pens[i]<=old_links) used[*pens[i]]=1;
    uint32_t nl=0;
    for (uint32_t i=1;i<=old_links;i++) {
        if (used[i]) {v->links[nl++]=v->links[i-1];link_map[i]=nl;}
        else free(v->links[i-1]);
    }
    v->nlinks=nl;
    for (uint32_t i=0;i<v->nstyles;i++) v->styles[i].link=v->styles[i].link<=old_links?link_map[v->styles[i].link]:0;
    for (int i=0;i<4;i++) *pens[i]=*pens[i]<=old_links?link_map[*pens[i]]:0;
    style_rehash(v,v->style_hash_cap);
out:
    free(m.styles);free(m.clusters);free(r.styles);free(r.clusters);free(used);free(link_map);
    v->pen_dirty=true;v->view_revision=0;v->revision++;
}
static void compact(struct vt *v) {
    compact_once(v);
    if ((v->nstyles>VT_STYLE_MAX/4*3 || v->nclusters>(1u<<20)/4*3 || v->nlinks>65536/4*3) && v->history_lines) {
        for (uint32_t n=(v->history_lines+1)/2;n--;) history_drop_oldest(v);
        compact_once(v);
    }
}
uint16_t vt_intern_style(struct vt *v,const struct vt_style *in) {
    struct vt_style k=*in;k.pad=0;
    for (int attempt=0;attempt<2;attempt++) {
        uint32_t h=style_hash(&k)&(v->style_hash_cap-1);
        while (v->style_hash[h]!=UINT32_MAX) {
            if (!memcmp(&v->styles[v->style_hash[h]],&k,sizeof k)) return (uint16_t)v->style_hash[h];
            h=(h+1)&(v->style_hash_cap-1);
        }
        if (v->nstyles<VT_STYLE_MAX) {
            if (v->nstyles==v->styles_cap) {
                uint32_t cap=v->styles_cap*2;struct vt_style *g=realloc(v->styles,cap*sizeof *g);
                if (!g) return 0;
                v->styles=g;v->styles_cap=cap;
            }
            v->styles[v->nstyles]=k;v->style_hash[h]=v->nstyles;
            uint16_t id=(uint16_t)v->nstyles++;
            if (v->nstyles*2>v->style_hash_cap) style_rehash(v,v->style_hash_cap*2);
            return id;
        }
        compact(v);
    }
    return 0;    /* 65535 looks on screen at once: the default is the honest fallback */
}
uint32_t vt_intern_link(struct vt *v,const char *s) {
    if (!s || !*s) return 0;
    for (uint32_t i=v->nlinks;i-->0;) if (!strcmp(v->links[i],s)) return i+1;
    if (v->nlinks>=65536) compact(v);
    if (v->nlinks==v->links_cap) {
        uint32_t cap=v->links_cap?v->links_cap*2:16;char **g=realloc(v->links,cap*sizeof *g);
        if (!g) return 0;
        v->links=g;v->links_cap=cap;
    }
    char *copy=malloc(strlen(s)+1);
    if (!copy) return 0;
    strcpy(copy,s);
    v->links[v->nlinks++]=copy;
    return v->nlinks;
}
const char *vt_link_uri(const struct vt *v,uint32_t link) {
    if (!link || link>v->nlinks) return NULL;
    const char *s=v->links[link-1],*semi=strchr(s,';');
    return semi?semi+1:s;
}
static uint32_t intern_cluster(struct vt *v,const uint32_t *cps,unsigned n) {
    if (v->nclusters>=(1u<<20)) compact(v);
    for (uint32_t i=v->nclusters,seen=0;i-->0 && seen<64;seen++)
        if (v->cluster_len[i]==n && !memcmp(v->clusters[i],cps,n*sizeof *cps)) return i;
    if (v->nclusters==v->clusters_cap) {
        uint32_t cap=v->clusters_cap?v->clusters_cap*2:64;
        uint32_t (*g)[8]=realloc(v->clusters,cap*sizeof *g);
        if (!g) return UINT32_MAX;
        v->clusters=g;
        uint8_t *l=realloc(v->cluster_len,cap);
        if (!l) return UINT32_MAX;
        v->cluster_len=l;v->clusters_cap=cap;
    }
    memcpy(v->clusters[v->nclusters],cps,n*sizeof *cps);v->cluster_len[v->nclusters]=(uint8_t)n;
    return v->nclusters++;
}

uint16_t vt_pen_id(struct vt *v) {
    if (v->pen_dirty) {v->pen_id=vt_intern_style(v,&vt_active(v)->cursor.pen);v->pen_dirty=false;}
    return v->pen_id;
}
/* An erased cell keeps only the pen's background, as xterm's BCE does: no
 * colour of its own, no attributes, no underline, no link. */
uint16_t vt_blank_id(struct vt *v) {
    struct vt_style s={0};s.bg=vt_active(v)->cursor.pen.bg;
    return s.bg==VT_COLOR_DEFAULT?0:vt_intern_style(v,&s);
}
struct vt_cell vt_blank_cell(struct vt *v) {
    struct vt_cell c={' ',vt_blank_id(v),1,0};
    return c;
}
const struct vt_style *vt_style_of(const struct vt *v,const struct vt_cell *c) {
    return &v->styles[c->style<v->nstyles?c->style:0];
}
size_t vt_cell_text(const struct vt *v,const struct vt_cell *c,char out[64]) {
    size_t n=0;
    if (c->flags&VT_CELL_CLUSTER && c->cp<v->nclusters) {
        for (unsigned i=0;i<v->cluster_len[c->cp];i++) n+=vt_encode(v->clusters[c->cp][i],out+n);
    } else n=vt_encode(c->cp>=' '?c->cp:' ',out);
    out[n]=0;
    return n;
}

/* --- rows ------------------------------------------------------------------------ */

static bool alloc_rows(struct vt_screen *s,int rows,int cap) {
    if (!s->rows) {s->rows=calloc(VT_MAX_ROWS,sizeof *s->rows);if(!s->rows)return false;}
    for (int y=0;y<rows;y++) {
        if (!s->rows[y]) {s->rows[y]=calloc(1,sizeof **s->rows);if(!s->rows[y])return false;}
        struct vt_cell *g=realloc(s->rows[y]->cells,(size_t)cap*sizeof *g);
        if (!g) return false;
        s->rows[y]->cells=g;
    }
    return true;
}
static void clear_row(struct vt_row *r,int cols,struct vt_cell blank) {
    for (int x=0;x<cols;x++) r->cells[x]=blank;
    r->wrapped=r->pad=false;
}
static void init_screen(struct vt *v,struct vt_screen *s) {
    for (int y=0;y<v->rows;y++) clear_row(s->rows[y],v->row_cap,plain_blank);
    memset(&s->cursor,0,sizeof s->cursor);
    s->saved=s->cursor;
    s->top=0;s->bottom=v->rows-1;
}
/* Overwriting half of a wide character blanks the other half. */
static void unpair(struct vt *v,struct vt_row *r,int x) {
    if (r->cells[x].width==0 && x>0) r->cells[x-1]=vt_blank_cell(v);
    else if (r->cells[x].width==2 && x+1<v->cols) r->cells[x+1]=vt_blank_cell(v);
}
/* No half of a wide character left without the other after a shift or resize. */
static void normalize_row(struct vt *v,struct vt_row *r,int cols) {
    for (int x=0;x<cols;x++) {
        if (r->cells[x].width==2) {
            if (x+1>=cols || r->cells[x+1].width!=0) r->cells[x]=vt_blank_cell(v); else x++;
        } else if (r->cells[x].width==0) r->cells[x]=vt_blank_cell(v);
    }
}

/* --- the grid ---------------------------------------------------------------------- */

struct vt_screen *vt_active(struct vt *v) { return v->alt?&v->alternate:&v->primary; }

void vt_reply(struct vt *v,const char *s) { if (v->reply) v->reply(v->reply_context,s,strlen(s)); }

void vt_scroll(struct vt *v,int top,int bottom,int count,bool up) {
    struct vt_screen *s=vt_active(v);
    count=vt_clamp(count,1,bottom-top+1);
    struct vt_cell blank=vt_blank_cell(v);
    bool keep=up && !v->alt && top==0 && bottom==v->rows-1;
    while (count--) {
        struct vt_row *r=s->rows[up?top:bottom];
        if (keep) history_push(v,r,v->cols);
        if (up) memmove(s->rows+top,s->rows+top+1,(size_t)(bottom-top)*sizeof *s->rows);
        else memmove(s->rows+top+1,s->rows+top,(size_t)(bottom-top)*sizeof *s->rows);
        s->rows[up?bottom:top]=r;
        clear_row(r,v->cols,blank);
    }
    v->revision++;
}
void vt_index(struct vt *v,bool reverse) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;c->wrap=false;
    if (reverse) {
        if (c->y==s->top) vt_scroll(v,s->top,s->bottom,1,false); else if (c->y>0) c->y--;
    } else {
        if (c->y==s->bottom) vt_scroll(v,s->top,s->bottom,1,true); else if (c->y<v->rows-1) c->y++;
    }
    v->revision++;
}
void vt_cursor_to(struct vt *v,int x,int y) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;
    c->x=vt_clamp(x,0,v->cols-1);
    c->y=vt_clamp(y,c->origin?s->top:0,c->origin?s->bottom:v->rows-1);
    c->wrap=false;v->revision++;
}
void vt_erase(struct vt *v,int row,int from,int to) {
    struct vt_row *r=vt_active(v)->rows[row];
    from=vt_clamp(from,0,v->cols);to=vt_clamp(to,0,v->cols);
    if (from>=to) return;
    unpair(v,r,from);
    unpair(v,r,to-1);
    struct vt_cell b=vt_blank_cell(v);
    for (int x=from;x<to;x++) r->cells[x]=b;
    if (to==v->cols) r->wrapped=r->pad=false;
    v->revision++;
}
void vt_insert_cells(struct vt *v,int n) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;struct vt_row *r=s->rows[c->y];
    n=vt_clamp(n,1,v->cols-c->x);
    unpair(v,r,c->x);
    memmove(r->cells+c->x+n,r->cells+c->x,(size_t)(v->cols-c->x-n)*sizeof *r->cells);
    struct vt_cell b=vt_blank_cell(v);
    for (int x=c->x;x<c->x+n;x++) r->cells[x]=b;
    normalize_row(v,r,v->cols);
    r->wrapped=r->pad=false;c->wrap=false;v->revision++;
}
void vt_delete_cells(struct vt *v,int n) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;struct vt_row *r=s->rows[c->y];
    n=vt_clamp(n,1,v->cols-c->x);
    unpair(v,r,c->x);
    memmove(r->cells+c->x,r->cells+c->x+n,(size_t)(v->cols-c->x-n)*sizeof *r->cells);
    struct vt_cell b=vt_blank_cell(v);
    for (int x=v->cols-n;x<v->cols;x++) r->cells[x]=b;
    normalize_row(v,r,v->cols);
    r->wrapped=r->pad=false;c->wrap=false;v->revision++;
}

static uint32_t dec_graphics(uint32_t c) {
    static const uint32_t map[31]={0x25c6,0x2592,0x2409,0x240c,0x240d,0x240a,0xb0,0xb1,0x2424,0x240b,
        0x2518,0x2510,0x250c,0x2514,0x253c,0x23ba,0x23bb,0x2500,0x23bc,0x23bd,0x251c,0x2524,0x2534,0x252c,0x2502,
        0x2264,0x2265,0x3c0,0x2260,0xa3,0xb7};
    return c>=0x60 && c<=0x7e?map[c-0x60]:c;
}
/* A combining mark joins the cell before the cursor (across a wrap). */
static void combine(struct vt *v,uint32_t mark) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;
    int y=c->y,x=c->wrap?c->x:c->x-1;
    if (x<0) {
        if (y==0 || !s->rows[y-1]->wrapped) return;
        y--;x=v->cols-1-(s->rows[y]->pad?1:0);
    }
    if (!s->rows[y]->cells[x].width && x>0) x--;
    struct vt_cell *cell=&s->rows[y]->cells[x];
    uint32_t cps[8];unsigned n;
    if (cell->flags&VT_CELL_CLUSTER && cell->cp<v->nclusters) {n=v->cluster_len[cell->cp];memcpy(cps,v->clusters[cell->cp],n*sizeof *cps);}
    else {cps[0]=cell->cp;n=1;}
    if (n>=8) return;                  /* a cell holds seven marks; more are dropped */
    cps[n++]=mark;
    uint32_t id=intern_cluster(v,cps,n);
    if (id==UINT32_MAX) return;
    cell=&s->rows[y]->cells[x];        /* a compaction may have renumbered it: the new id stands */
    cell->cp=id;cell->flags|=VT_CELL_CLUSTER;
    v->revision++;
}
void vt_print(struct vt *v,uint32_t cp) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;
    if (c->graphics[c->charset&3]) cp=dec_graphics(cp);
    int w=vt_width(cp);
    if (w<0) return;
    if (w==0) {combine(v,cp);return;}
    v->last_cp=cp;
    if (c->wrap && v->autowrap) {
        s->rows[c->y]->wrapped=true;s->rows[c->y]->pad=false;
        vt_index(v,false);c->x=0;
    }
    c->wrap=false;
    if (w==2 && c->x==v->cols-1) {
        if (v->autowrap) {
            vt_erase(v,c->y,c->x,c->x+1);
            s->rows[c->y]->wrapped=true;s->rows[c->y]->pad=true;
            vt_index(v,false);c->x=0;
        } else {cp=0xfffd;w=1;}
    }
    if (v->insert_mode) vt_insert_cells(v,w);
    struct vt_row *r=s->rows[c->y];
    unpair(v,r,c->x);
    if (w==2) unpair(v,r,c->x+1);
    uint16_t pen=vt_pen_id(v);
    r->cells[c->x]=(struct vt_cell){cp,pen,(uint8_t)w,0};
    if (w==2) r->cells[c->x+1]=(struct vt_cell){' ',pen,0,0};
    c->x+=w;
    if (c->x>=v->cols) {c->x=v->cols-1;c->wrap=v->autowrap;}
    v->revision++;
}
void vt_save_cursor(struct vt *v) { struct vt_screen *s=vt_active(v);s->saved=s->cursor; }
void vt_restore_cursor(struct vt *v) {
    struct vt_screen *s=vt_active(v);s->cursor=s->saved;
    s->cursor.x=vt_clamp(s->cursor.x,0,v->cols-1);s->cursor.y=vt_clamp(s->cursor.y,0,v->rows-1);
    v->pen_dirty=true;v->revision++;
}
void vt_alternate(struct vt *v,bool on,bool save,bool clear) {
    if (on && !v->alt) {
        if (save) vt_save_cursor(v);
        v->alt_saved=save;
        if (clear) for (int y=0;y<v->rows;y++) clear_row(v->alternate.rows[y],v->cols,plain_blank);
        v->alternate.cursor=v->primary.cursor;
        v->alternate.top=0;v->alternate.bottom=v->rows-1;
        v->alt=true;
    } else if (!on && v->alt) {
        if (clear) for (int y=0;y<v->rows;y++) clear_row(v->alternate.rows[y],v->cols,plain_blank);
        v->alt=false;
        if (v->alt_saved) vt_restore_cursor(v);
        v->alt_saved=false;
    }
    v->pen_dirty=true;v->revision++;
}
void vt_fill_e(struct vt *v) {
    struct vt_screen *s=vt_active(v);
    for (int y=0;y<v->rows;y++) {
        for (int x=0;x<v->cols;x++) s->rows[y]->cells[x]=(struct vt_cell){'E',0,1,0};
        s->rows[y]->wrapped=s->rows[y]->pad=false;
    }
    s->top=0;s->bottom=v->rows-1;s->cursor.origin=false;
    vt_cursor_to(v,0,0);
}

/* --- lifetime ------------------------------------------------------------------------ */

static void defaults(struct vt *v) {
    v->autowrap=v->cursor_visible=true;
    v->prompt_row=-1;
    for (int i=0;i<VT_MAX_COLS;i++) v->tabs[i]=i && i%8==0;
}
bool vt_init(struct vt *v,int cols,int rows,vt_reply_fn reply,void *context) {
    memset(v,0,sizeof *v);
    v->cols=vt_clamp(cols,2,VT_MAX_COLS);v->rows=vt_clamp(rows,2,VT_MAX_ROWS);v->row_cap=v->cols;
    v->reply=reply;v->reply_context=context;
    v->alt_sends_escape=true;
    v->history_limit=VT_HISTORY_BYTES;
    v->styles_cap=256;v->styles=calloc(v->styles_cap,sizeof *v->styles);v->nstyles=1;
    v->tabs=calloc(VT_MAX_COLS,sizeof *v->tabs);
    if (!v->styles || !v->tabs) {vt_free(v);return false;}
    style_rehash(v,512);
    if (!v->style_hash || !alloc_rows(&v->primary,v->rows,v->row_cap) || !alloc_rows(&v->alternate,v->rows,v->row_cap)) {
        vt_free(v);return false;
    }
    for (unsigned i=0;i<256;i++) v->base.color[i]=vt_default_color(i);
    v->base.fg=0xd9dfe7;v->base.bg=0x1c1f24;v->base.cursor=0x6abec5;
    init_screen(v,&v->primary);init_screen(v,&v->alternate);
    defaults(v);
    v->revision=1;
    return true;
}
void vt_free(struct vt *v) {
    struct vt_screen *screens[2]={&v->primary,&v->alternate};
    for (int i=0;i<2;i++) if (screens[i]->rows) {
        for (int y=0;y<VT_MAX_ROWS;y++) if (screens[i]->rows[y]) {free(screens[i]->rows[y]->cells);free(screens[i]->rows[y]);}
        free(screens[i]->rows);screens[i]->rows=NULL;
    }
    if (v->history) {while (v->history_lines) history_drop_oldest(v);free(v->history);v->history=NULL;}
    v->history_cap=0;
    for (uint32_t i=0;i<v->nlinks;i++) free(v->links[i]);
    free(v->links);free(v->styles);free(v->style_hash);free(v->clusters);free(v->cluster_len);free(v->tabs);free(v->view_cells);
    v->links=NULL;v->styles=NULL;v->style_hash=NULL;v->clusters=NULL;v->cluster_len=NULL;v->tabs=NULL;v->view_cells=NULL;
    v->nlinks=v->nstyles=v->nclusters=0;
}
void vt_reset(struct vt *v) {
    vt_clear_history(v);
    v->alt=v->alt_saved=false;
    init_screen(v,&v->primary);init_screen(v,&v->alternate);
    v->application_cursor=v->keypad_application=v->bracketed_paste=v->insert_mode=v->newline_mode=false;
    v->reverse_video=v->focus_events=v->sync_output=v->alternate_scroll=v->inband_resize=v->cursor_blink=false;
    v->cursor_style=0;
    v->mouse=VT_MOUSE_OFF;v->mouse_encoding=VT_MOUSE_ENC_DEFAULT;
    memset(&v->over,0,sizeof v->over);memset(v->over_set,0,sizeof v->over_set);
    v->over_fg=v->over_bg=v->over_cursor=false;
    v->title[0]=v->icon[0]=0;v->title_depth=0;v->clipboard_pending=false;v->clipboard_len=0;
    v->shell_ready=v->edit_shell=false;
    v->nxtsave=0;v->last_cp=0;
    defaults(v);
    v->pen_dirty=true;v->revision++;
}

/* --- reflow ----------------------------------------------------------------------------
 * The primary screen's rows are joined back into logical lines (an open
 * scrollback line is taken back to join its continuation), the places of the
 * cursor and of the prompt in them noted, and the lines laid out again at the
 * new width. Rows that do not fit above the cursor go to the scrollback; a
 * screen that was full when it grew taller shows scrollback lines again. */

struct cells { struct vt_cell *c; uint32_t n, cap; };
static bool cells_add(struct cells *l,const struct vt_cell *c,uint32_t n) {
    if (l->n+n>l->cap) {
        uint32_t cap=l->cap?l->cap:64;
        while (cap<l->n+n) cap*=2;
        struct vt_cell *g=realloc(l->c,cap*sizeof *g);
        if (!g) return false;
        l->c=g;l->cap=cap;
    }
    if (n) memcpy(l->c+l->n,c,n*sizeof *c);
    l->n+=n;
    return true;
}
struct rows { struct vt_row *r; int n, cap; };
static struct vt_row *rows_add(struct rows *o,int cols) {
    if (o->n==o->cap) {
        int cap=o->cap?o->cap*2:64;
        struct vt_row *g=realloc(o->r,(size_t)cap*sizeof *g);
        if (!g) return NULL;
        o->r=g;o->cap=cap;
    }
    struct vt_row *row=&o->r[o->n];
    row->cells=malloc((size_t)cols*sizeof *row->cells);
    if (!row->cells) return NULL;
    clear_row(row,cols,plain_blank);
    o->n++;
    return row;
}
static void rows_free(struct rows *o) { for (int i=0;i<o->n;i++) free(o->r[i].cells);free(o->r); }
/* A place in a logical line, found again after it is laid out: `off` is a
 * cell offset (UINT32_MAX for none); one just past a full row's end is that
 * row's last column with the wrap pending. */
struct spot { uint32_t off; int row, col; bool wrap; };
static bool layout(struct rows *o,const struct vt_cell *cells,uint32_t n,int cols,struct spot *spots,int nspots) {
    uint32_t i=0;
    do {
        struct vt_row *row=rows_add(o,cols);
        if (!row) return false;
        int x=0;
        while (i<n) {
            for (int k=0;k<nspots;k++) if (spots[k].off==i) {spots[k].row=o->n-1;spots[k].col=x;spots[k].wrap=false;}
            int w=cells[i].width;
            if (!w) {i++;continue;}
            if (x+w>cols) {row->pad=x<cols;break;}
            row->cells[x]=cells[i];
            if (w==2) row->cells[x+1]=(struct vt_cell){' ',cells[i].style,0,0};
            x+=w;i++;
        }
        if (i<n) row->wrapped=true;
        else for (int k=0;k<nspots;k++) if (spots[k].off==n) {
            spots[k].row=o->n-1;spots[k].wrap=x>=cols;spots[k].col=x>=cols?cols-1:x;
        }
    } while (i<n);
    return true;
}
static void place(struct vt_row *dst,const struct vt_row *src,int cols) {
    memcpy(dst->cells,src->cells,(size_t)cols*sizeof *src->cells);
    dst->wrapped=src->wrapped;dst->pad=src->pad;
}
static bool reflow_primary(struct vt *v,int cols,int rows,int old_cols,int old_rows) {
    struct vt_screen *s=&v->primary;
    int last=s->cursor.y;
    for (int y=old_rows-1;y>last;y--)
        if (row_length(s->rows[y],old_cols) || s->rows[y]->wrapped) {last=y;break;}
    bool was_full=last==old_rows-1;
    int64_t prompt_y=v->prompt_row>=0?v->prompt_row-(int64_t)v->scrolls:-1;

    struct cells *lines=calloc((size_t)last+2,sizeof *lines);
    if (!lines) return false;
    uint32_t nlines=1,cur_line=0,pr_line=UINT32_MAX;
    struct spot cur={UINT32_MAX,0,0,false},pr={UINT32_MAX,-1,0,false};
    if (v->history_lines && history_line(v,v->history_lines-1)->open) {
        struct vt_hline *h=history_line(v,v->history_lines-1);
        if (!cells_add(&lines[0],h->cells,h->n)) {free(lines);return false;}
        history_drop_newest(v);
    }
    for (int y=0;y<=last;y++) {
        struct vt_row *r=s->rows[y];
        struct cells *l=&lines[nlines-1];
        uint32_t n=row_length(r,old_cols);
        if (y==s->cursor.y) {
            uint32_t at=(uint32_t)s->cursor.x+(s->cursor.wrap?1:0);
            if (n<at) n=at;
            cur_line=nlines-1;cur.off=l->n+at;
        }
        if (y==prompt_y) {pr_line=nlines-1;pr.off=l->n;}
        if (!cells_add(l,r->cells,n)) goto fail;
        if (!r->wrapped && y<last) nlines++;
    }

    struct rows all={0};
    for (uint32_t li=0;li<nlines;li++) {
        struct spot *sp[2];int ns=0;
        if (li==cur_line) sp[ns++]=&cur;
        if (li==pr_line) sp[ns++]=&pr;
        struct spot local[2];
        for (int k=0;k<ns;k++) local[k]=*sp[k];
        if (!layout(&all,lines[li].c,lines[li].n,cols,local,ns)) {rows_free(&all);goto fail;}
        for (int k=0;k<ns;k++) *sp[k]=local[k];
    }

    /* too many: rows above the cursor go to the scrollback; past that, the
     * bottom ones are cut */
    int first=0;
    if (all.n>rows) {
        first=all.n-rows<cur.row?all.n-rows:cur.row;
        for (int k=0;k<first;k++) history_push(v,&all.r[k],cols);
    }
    int keep=all.n-first<rows?all.n-first:rows;
    /* too few, on a screen that was full: scrollback lines come back */
    struct rows pulled={0};
    while (was_full && v->history_lines && keep+pulled.n<rows) {
        struct vt_hline *h=history_line(v,v->history_lines-1);
        if (keep+pulled.n+(int)line_rows(h->cells,h->n,cols)>rows) break;
        struct rows one={0};
        bool ok=layout(&one,h->cells,h->n,cols,NULL,0);
        for (int k=one.n;ok && k-->0;) {      /* `pulled` runs bottom-up */
            struct vt_row *dst=rows_add(&pulled,cols);
            if (!dst) ok=false; else place(dst,&one.r[k],cols);
        }
        rows_free(&one);
        if (!ok) break;
        history_drop_newest(v);
    }
    int y=0;
    for (int k=pulled.n;k-->0;y++) place(s->rows[y],&pulled.r[k],cols);
    for (int k=first;k<first+keep;k++,y++) place(s->rows[y],&all.r[k],cols);
    for (;y<rows;y++) clear_row(s->rows[y],cols,plain_blank);
    s->cursor.y=vt_clamp(cur.row-first+pulled.n,0,rows-1);
    s->cursor.x=vt_clamp(cur.col,0,cols-1);
    s->cursor.wrap=cur.wrap;
    if (pr.row>=0) v->prompt_row=(int64_t)v->scrolls+pr.row-first+pulled.n;
    else if (v->prompt_row>=0) v->prompt_row=(int64_t)v->scrolls-1;
    rows_free(&all);rows_free(&pulled);
    for (uint32_t i=0;i<nlines;i++) free(lines[i].c);
    free(lines);
    return true;
fail:
    for (uint32_t i=0;i<nlines;i++) free(lines[i].c);
    free(lines);
    return false;
}
static void rewrap_history(struct vt *v,int cols) {
    v->history_rows=0;
    for (uint32_t i=0;i<v->history_lines;i++) {
        struct vt_hline *l=history_line(v,i);
        l->rows=line_rows(l->cells,l->n,cols);
        v->history_rows+=l->rows;
    }
}
bool vt_resize(struct vt *v,int cols,int rows) {
    cols=vt_clamp(cols,2,VT_MAX_COLS);rows=vt_clamp(rows,2,VT_MAX_ROWS);
    if (cols==v->cols && rows==v->rows) return true;
    int old_cols=v->cols,old_rows=v->rows;
    int cap=cols>v->row_cap?cols:v->row_cap;
    int need=rows>old_rows?rows:old_rows;
    if (!alloc_rows(&v->primary,need,cap) || !alloc_rows(&v->alternate,need,cap)) return false;
    v->row_cap=cap;
    for (int y=0;y<old_rows;y++) for (int x=old_cols;x<cap;x++)
        v->primary.rows[y]->cells[x]=v->alternate.rows[y]->cells[x]=plain_blank;
    for (int y=old_rows;y<need;y++) {clear_row(v->primary.rows[y],cap,plain_blank);clear_row(v->alternate.rows[y],cap,plain_blank);}
    rewrap_history(v,cols);
    if (!reflow_primary(v,cols,rows,old_cols,old_rows)) return false;
    /* in the alternate screen with 1049, the cursor saved for the way back is
     * the primary one, which moved with its text */
    if (v->alt && v->alt_saved) {
        v->primary.saved.x=v->primary.cursor.x;v->primary.saved.y=v->primary.cursor.y;
        v->primary.saved.wrap=v->primary.cursor.wrap;
    }
    v->cols=cols;v->rows=rows;
    /* the alternate screen keeps its coordinates: clipped, padded, wide characters mended */
    for (int y=0;y<rows;y++) {
        struct vt_row *r=v->alternate.rows[y];
        if (cols<old_cols) r->wrapped=r->pad=false;
        normalize_row(v,r,cols);
    }
    rewrap_history(v,cols);
    struct vt_screen *screens[2]={&v->primary,&v->alternate};
    for (int i=0;i<2;i++) {
        struct vt_screen *s=screens[i];
        s->cursor.x=vt_clamp(s->cursor.x,0,cols-1);s->cursor.y=vt_clamp(s->cursor.y,0,rows-1);
        s->saved.x=vt_clamp(s->saved.x,0,cols-1);s->saved.y=vt_clamp(s->saved.y,0,rows-1);
        if (s->cursor.x!=cols-1) s->cursor.wrap=false;
        s->top=0;s->bottom=rows-1;
    }
    v->pen_dirty=true;v->view_revision=0;v->revision++;
    return true;
}

/* --- reading the picture ------------------------------------------------------------------ */

/* Row `row` of the view `back` rows into the scrollback. A scrollback row is
 * laid out at the current width into view_cells: the last one is kept, and
 * the line it came from, so reading a frame's rows in order walks the lines
 * once. The cache is the only thing a read writes. */
static const struct vt_cell *view_row(const struct vt *cv,int row,unsigned back,bool *wrapped) {
    struct vt *v=(struct vt *)cv;
    const struct vt_screen *s=v->alt?&v->alternate:&v->primary;
    if (v->alt) back=0;
    if (back>v->history_rows) back=v->history_rows;
    int64_t logical=(int64_t)v->history_rows+row-(int64_t)back;
    if (logical>=(int64_t)v->history_rows) {
        const struct vt_row *r=s->rows[logical-(int64_t)v->history_rows];
        if (wrapped) *wrapped=r->wrapped;
        return r->cells;
    }
    if (!v->view_cells || v->view_cols<v->cols) {
        struct vt_cell *g=realloc(v->view_cells,(size_t)v->cols*sizeof *g);
        if (!g) return NULL;
        v->view_cells=g;v->view_cols=v->cols;v->view_revision=0;
    }
    bool fresh=v->view_revision==v->revision && v->view_line<v->history_lines;
    if (fresh && v->view_row==logical) {
        if (wrapped) *wrapped=logical-v->view_line_top<(int64_t)history_line(v,v->view_line)->rows-1;
        return v->view_cells;
    }
    /* the line holding that row: from the last one read, else from the newest */
    uint32_t i;int64_t top;
    if (fresh) {i=v->view_line;top=v->view_line_top;}
    else {i=v->history_lines-1;top=(int64_t)v->history_rows-history_line(v,i)->rows;}
    while (logical<top && i>0) {i--;top-=history_line(v,i)->rows;}
    while (logical>=top+(int64_t)history_line(v,i)->rows && i+1<v->history_lines) {top+=history_line(v,i)->rows;i++;}
    struct vt_hline *l=history_line(v,i);
    int64_t seg=logical-top;
    for (int x=0;x<v->cols;x++) v->view_cells[x]=plain_blank;
    int64_t r=0;int x=0;
    for (uint32_t j=0;j<l->n && r<=seg;j++) {
        int w=l->cells[j].width;
        if (!w) continue;
        if (x+w>v->cols) {r++;x=0;}
        if (r==seg) {
            v->view_cells[x]=l->cells[j];
            if (w==2) v->view_cells[x+1]=(struct vt_cell){' ',l->cells[j].style,0,0};
        }
        x+=w;
    }
    v->view_revision=v->revision;v->view_row=logical;v->view_line=i;v->view_line_top=top;
    if (wrapped) *wrapped=seg<(int64_t)l->rows-1;
    return v->view_cells;
}
const struct vt_cell *vt_view_cell(const struct vt *v,int row,int col,unsigned back) {
    if (row<0 || row>=v->rows || col<0 || col>=v->cols) return &plain_blank;
    const struct vt_cell *cells=view_row(v,row,back,NULL);
    return cells?&cells[col]:&plain_blank;
}
const struct vt_cell *vt_view_row(const struct vt *v,int row,unsigned back) {
    if (row<0 || row>=v->rows) return NULL;
    return view_row(v,row,back,NULL);
}
int64_t vt_row_id(const struct vt *v,int row,unsigned back) {
    if (v->alt) back=0;
    if (back>v->history_rows) back=v->history_rows;
    return (int64_t)v->scrolls-(int64_t)back+row;
}
const struct vt_cell *vt_row_cells(const struct vt *v,int64_t id,bool *wrapped) {
    int64_t rel=id-(int64_t)v->scrolls;
    bool w=false;const struct vt_cell *cells=NULL;
    if (rel>=0) {
        if (rel<v->rows) cells=view_row(v,(int)rel,0,&w);
    } else if (!v->alt && -rel<=(int64_t)v->history_rows) cells=view_row(v,0,(unsigned)-rel,&w);
    if (wrapped) *wrapped=cells && w;
    return cells;
}
int vt_row_in_view(const struct vt *v,int64_t id,unsigned back) {
    int64_t row=id-vt_row_id(v,0,back);
    return row>=0 && row<v->rows?(int)row:-1;
}
bool vt_view_wrapped(const struct vt *v,int row,unsigned back) {
    bool w=false;
    if (row>=0 && row<v->rows) view_row(v,row,back,&w);
    return w;
}
size_t vt_line_text(const struct vt *v,int row,unsigned back,char *dst,size_t cap) {
    size_t n=0;int last=-1;
    if (!cap) return 0;
    const struct vt_cell *cells=row>=0 && row<v->rows?view_row(v,row,back,NULL):NULL;
    if (cells) for (int x=0;x<v->cols;x++) {
        const struct vt_cell *c=&cells[x];
        if (c->width && (c->cp!=' ' || c->flags) && !(vt_style_of(v,c)->attrs&VT_HIDDEN)) last=x;
    }
    for (int x=0;x<=last;x++) {
        const struct vt_cell *c=&cells[x];
        if (!c->width) continue;
        char text[64];size_t k;
        if (vt_style_of(v,c)->attrs&VT_HIDDEN) {text[0]=' ';k=1;}
        else k=vt_cell_text(v,c,text);
        if (n+k>=cap) break;
        memcpy(dst+n,text,k);n+=k;
    }
    dst[n]=0;
    return n;
}

/* --- shell integration ---------------------------------------------------------------------- */

bool vt_prompt_erase(struct vt *v) {
    if (v->alt || !v->shell_ready || !v->edit_shell || !v->inband_resize || v->prompt_row<0) return false;
    int64_t y=v->prompt_row-(int64_t)v->scrolls;
    if (y<0) y=0;
    if (y>=v->rows) return false;
    struct vt_screen *s=&v->primary;
    for (int r=(int)y;r<v->rows;r++) clear_row(s->rows[r],v->cols,plain_blank);
    if (y>0) s->rows[y-1]->wrapped=s->rows[y-1]->pad=false;
    s->cursor.x=0;s->cursor.y=(int)y;s->cursor.wrap=false;
    v->revision++;
    return true;
}
void vt_prompt_here(struct vt *v) {
    if (!v->alt) v->prompt_row=(int64_t)v->scrolls+v->primary.cursor.y;
}
size_t vt_size_report(const struct vt *v,char out[48]) {
    int n=snprintf(out,48,"\033[48;%d;%d;%d;%dt",v->rows,v->cols,v->pixel_height,v->pixel_width);
    return n>0?(size_t)n:0;
}
