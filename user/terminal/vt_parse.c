/* vt_parse.c -- the byte stream to grid operations. See vt.h.
 *
 * A DEC-style parser (the states of vt100.net's "A parser for DEC's ANSI-
 * compatible video terminals"): C0 controls act anywhere, ESC/CSI/DCS
 * collect intermediates, a private marker and parameters -- `;` between
 * parameters, `:` between the sub-parameters of one (SGR 4:3, 38:2::r:g:b) --
 * and the final byte dispatches. OSC, DCS, SOS, PM and APC collect a string
 * up to ST or BEL. Text is UTF-8; a malformed sequence is one U+FFFD.
 *
 * What each sequence does is xterm's (ctlseqs), and docs/TERMINAL.md lists
 * the ones this terminal answers. */
#include "vt_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GROUND, ESCAPE, ESCAPE_INTERMEDIATE, CSI, CSI_IGNORE, STRING };
enum { STR_OSC=1, STR_DCS, STR_OTHER };     /* string_kind */

#define VERSION "EmbLinkTerminal(1.0)"

static int param(const struct vt *v,unsigned i,int fallback) {
    return i<v->nparam && v->params[i]>0?v->params[i]:fallback;
}
static int param0(const struct vt *v,unsigned i) { return i<v->nparam && v->params[i]>0?v->params[i]:0; }
static void replyf(struct vt *v,const char *fmt,...) __attribute__((format(printf,2,3)));
static void replyf(struct vt *v,const char *fmt,...) {
    char buf[512];va_list ap;va_start(ap,fmt);int n=vsnprintf(buf,sizeof buf,fmt,ap);va_end(ap);
    if (n>0 && v->reply) v->reply(v->reply_context,buf,(size_t)n<sizeof buf?(size_t)n:sizeof buf-1);
}

/* --- SGR ---------------------------------------------------------------------- */

/* An extended colour starting at params[i] (38, 48 or 58): the colon form
 * keeps its operands as sub-parameters, the semicolon form as the next
 * parameters. Returns the parameters used after i; *out stays unchanged for
 * a malformed one. */
static unsigned extended_color(const struct vt *v,unsigned i,uint32_t *out) {
    unsigned subs=0;
    while (i+1+subs<v->nparam && v->subparam[i+1+subs]) subs++;
    const int *p=&v->params[i+1];
    int kind,r,g,b;
    if (subs) {
        kind=p[0];
        if (kind==5 && subs>=2) {if (p[1]>=0 && p[1]<256) *out=VT_COLOR_INDEX(p[1]);}
        else if (kind==2 && subs>=4) {
            /* 38:2:<colour space>:r:g:b, or 38:2:r:g:b as some programs write it */
            const int *c=subs>=5?p+2:p+1;
            r=c[0]<0?0:c[0];g=c[1]<0?0:c[1];b=c[2]<0?0:c[2];
            if (r<256 && g<256 && b<256) *out=VT_COLOR_RGB(((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b);
        }
        return subs;
    }
    if (i+1>=v->nparam) return 0;
    kind=p[0];
    if (kind==5) {
        if (i+2>=v->nparam) return v->nparam-i-1;
        if (p[1]>=0 && p[1]<256) *out=VT_COLOR_INDEX(p[1]);
        return 2;
    }
    if (kind==2) {
        if (i+4>=v->nparam) return v->nparam-i-1;
        r=p[1]<0?0:p[1];g=p[2]<0?0:p[2];b=p[3]<0?0:p[3];
        if (r<256 && g<256 && b<256) *out=VT_COLOR_RGB(((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b);
        return 4;
    }
    return v->nparam-i-1;        /* unknown kind: the rest cannot be read as attributes */
}
static void sgr(struct vt *v) {
    struct vt_style *pen=&vt_active(v)->cursor.pen;
    uint32_t link=pen->link;
    if (!v->nparam) {v->nparam=1;v->params[0]=0;}
    for (unsigned i=0;i<v->nparam;i++) {
        if (v->subparam[i]) continue;     /* an operand of something we did not know */
        int p=v->params[i]<0?0:v->params[i];
        unsigned subs=0;
        while (i+1+subs<v->nparam && v->subparam[i+1+subs]) subs++;
        switch (p) {
        case 0: *pen=(struct vt_style){0};pen->link=link;break;
        case 1: pen->attrs|=VT_BOLD;break;
        case 2: pen->attrs|=VT_DIM;break;
        case 3: pen->attrs|=VT_ITALIC;break;
        case 4:
            if (subs) {int s=v->params[i+1]<0?0:v->params[i+1];if (s<=VT_UL_DASHED) pen->underline=(uint8_t)s;}
            else pen->underline=VT_UL_SINGLE;
            break;
        case 5: pen->attrs|=VT_BLINK;break;
        case 6: pen->attrs|=VT_RAPID;break;
        case 7: pen->attrs|=VT_INVERSE;break;
        case 8: pen->attrs|=VT_HIDDEN;break;
        case 9: pen->attrs|=VT_STRIKE;break;
        case 21: pen->underline=VT_UL_DOUBLE;break;
        case 22: pen->attrs&=(uint16_t)~(VT_BOLD|VT_DIM);break;
        case 23: pen->attrs&=(uint16_t)~VT_ITALIC;break;
        case 24: pen->underline=VT_UL_NONE;break;
        case 25: pen->attrs&=(uint16_t)~(VT_BLINK|VT_RAPID);break;
        case 27: pen->attrs&=(uint16_t)~VT_INVERSE;break;
        case 28: pen->attrs&=(uint16_t)~VT_HIDDEN;break;
        case 29: pen->attrs&=(uint16_t)~VT_STRIKE;break;
        case 38: i+=extended_color(v,i,&pen->fg);continue;
        case 39: pen->fg=VT_COLOR_DEFAULT;break;
        case 48: i+=extended_color(v,i,&pen->bg);continue;
        case 49: pen->bg=VT_COLOR_DEFAULT;break;
        case 53: pen->attrs|=VT_OVERLINE;break;
        case 55: pen->attrs&=(uint16_t)~VT_OVERLINE;break;
        case 58: i+=extended_color(v,i,&pen->ul);continue;
        case 59: pen->ul=VT_COLOR_DEFAULT;break;
        default:
            if (p>=30 && p<=37) pen->fg=VT_COLOR_INDEX(p-30);
            else if (p>=40 && p<=47) pen->bg=VT_COLOR_INDEX(p-40);
            else if (p>=90 && p<=97) pen->fg=VT_COLOR_INDEX(p-90+8);
            else if (p>=100 && p<=107) pen->bg=VT_COLOR_INDEX(p-100+8);
            break;                  /* fonts, fraktur, frames, ideograms: not drawn */
        }
        i+=subs;
    }
    v->pen_dirty=true;
}
/* The pen as SGR parameters, for DECRQSS. */
static void pen_sgr(const struct vt_style *pen,char *out,size_t cap) {
    size_t n=(size_t)snprintf(out,cap,"0");
#define ADD(...) do { if (n<cap) n+=(size_t)snprintf(out+n,cap-n,__VA_ARGS__); } while (0)
    static const struct { uint16_t bit; int code; } attrs[]={{VT_BOLD,1},{VT_DIM,2},{VT_ITALIC,3},{VT_BLINK,5},
        {VT_RAPID,6},{VT_INVERSE,7},{VT_HIDDEN,8},{VT_STRIKE,9},{VT_OVERLINE,53}};
    for (size_t i=0;i<sizeof attrs/sizeof *attrs;i++) if (pen->attrs&attrs[i].bit) ADD(";%d",attrs[i].code);
    if (pen->underline==VT_UL_SINGLE) ADD(";4");
    else if (pen->underline) ADD(";4:%d",pen->underline);
    const uint32_t colors[3]={pen->fg,pen->bg,pen->ul};
    for (int k=0;k<3;k++) {
        uint32_t c=colors[k];
        if (VT_COLOR_KIND(c)==1) {
            unsigned i=c&0xff;
            if (k<2 && i<8) ADD(";%u",(k?40:30)+i);
            else if (k<2 && i<16) ADD(";%u",(k?100:90)+i-8);
            else ADD(";%d:5:%u",k==0?38:k==1?48:58,i);
        } else if (VT_COLOR_KIND(c)==2) ADD(";%d:2::%u:%u:%u",k==0?38:k==1?48:58,(c>>16)&255,(c>>8)&255,c&255);
    }
#undef ADD
}

/* --- modes ----------------------------------------------------------------------- */

/* A DEC private mode's state for DECRQM: 1 set, 2 reset, 3/4 permanently
 * so, 0 not recognised. */
static int dec_mode(const struct vt *v,int p) {
#define B(x) ((x)?1:2)
    switch (p) {
    case 1: return B(v->application_cursor);
    case 5: return B(v->reverse_video);
    case 6: return B(vt_active((struct vt *)v)->cursor.origin);
    case 7: return B(v->autowrap);
    case 8: return 3;                 /* auto-repeat: the keyboard's */
    case 9: return B(v->mouse==VT_MOUSE_X10);
    case 12: return B(v->cursor_blink);
    case 25: return B(v->cursor_visible);
    case 47: case 1047: case 1049: return B(v->alt);
    case 66: return B(v->keypad_application);
    case 67: return B(v->backspace_sends_bs);
    case 1000: return B(v->mouse==VT_MOUSE_NORMAL);
    case 1002: return B(v->mouse==VT_MOUSE_BUTTON);
    case 1003: return B(v->mouse==VT_MOUSE_ANY);
    case 1004: return B(v->focus_events);
    case 1005: return B(v->mouse_encoding==VT_MOUSE_ENC_UTF8);
    case 1006: return B(v->mouse_encoding==VT_MOUSE_ENC_SGR);
    case 1007: return B(v->alternate_scroll);
    case 1015: return B(v->mouse_encoding==VT_MOUSE_ENC_URXVT);
    case 1016: return B(v->mouse_encoding==VT_MOUSE_ENC_SGR_PIXELS);
    case 1036: case 1039: return B(v->alt_sends_escape);
    case 1048: return 1;
    case 2004: return B(v->bracketed_paste);
    case 2026: return B(v->sync_output);
    case 2048: return B(v->inband_resize);
    default: return 0;
    }
#undef B
}
static void set_mouse(struct vt *v,enum vt_mouse_mode m,bool on) {
    if (on) v->mouse=m; else if (v->mouse==m) v->mouse=VT_MOUSE_OFF;
}
static void set_encoding(struct vt *v,enum vt_mouse_encoding e,bool on) {
    if (on) v->mouse_encoding=e; else if (v->mouse_encoding==e) v->mouse_encoding=VT_MOUSE_ENC_DEFAULT;
}
static void set_dec_mode(struct vt *v,int p,bool on) {
    struct vt_screen *s=vt_active(v);
    switch (p) {
    case 1: v->application_cursor=on;break;
    case 5: v->reverse_video=on;break;
    case 6: s->cursor.origin=on;vt_cursor_to(v,0,on?s->top:0);break;
    case 7: v->autowrap=on;s->cursor.wrap=false;break;
    case 9: set_mouse(v,VT_MOUSE_X10,on);break;
    case 12: v->cursor_blink=on;break;
    case 25: v->cursor_visible=on;break;
    case 47: vt_alternate(v,on,false,false);break;
    case 1047: vt_alternate(v,on,false,!on);break;        /* cleared on the way out */
    case 1048: if (on) vt_save_cursor(v); else vt_restore_cursor(v);break;
    case 1049: vt_alternate(v,on,true,on);break;          /* cleared on the way in */
    case 66: v->keypad_application=on;break;
    case 67: v->backspace_sends_bs=on;break;
    case 1000: set_mouse(v,VT_MOUSE_NORMAL,on);break;
    case 1002: set_mouse(v,VT_MOUSE_BUTTON,on);break;
    case 1003: set_mouse(v,VT_MOUSE_ANY,on);break;
    case 1004: v->focus_events=on;break;
    case 1005: set_encoding(v,VT_MOUSE_ENC_UTF8,on);break;
    case 1006: set_encoding(v,VT_MOUSE_ENC_SGR,on);break;
    case 1007: v->alternate_scroll=on;break;
    case 1015: set_encoding(v,VT_MOUSE_ENC_URXVT,on);break;
    case 1016: set_encoding(v,VT_MOUSE_ENC_SGR_PIXELS,on);break;
    case 1036: case 1039: v->alt_sends_escape=on;break;
    case 2004: v->bracketed_paste=on;break;
    case 2026: v->sync_output=on;break;
    case 2048:
        v->inband_resize=on;
        if (on && v->reply) {char r[48];size_t n=vt_size_report(v,r);v->reply(v->reply_context,r,n);}
        break;
    default: break;
    }
}
static void modes(struct vt *v,bool on) {
    for (unsigned i=0;i<v->nparam;i++) {
        int p=v->params[i];
        if (v->private_marker=='?') set_dec_mode(v,p,on);
        else if (!v->private_marker) {
            if (p==4) v->insert_mode=on;
            else if (p==20) v->newline_mode=on;
        }
    }
    v->revision++;
}
/* XTSAVE / XTRESTORE: DEC private modes kept by number. */
static void save_modes(struct vt *v) {
    for (unsigned i=0;i<v->nparam;i++) {
        int p=v->params[i],state=dec_mode(v,p);
        if (state!=1 && state!=2) continue;
        unsigned k=0;
        while (k<v->nxtsave && v->xtsave[k][0]!=p) k++;
        if (k==v->nxtsave) {if (v->nxtsave==16) continue;v->nxtsave++;}
        v->xtsave[k][0]=p;v->xtsave[k][1]=state==1;
    }
}
static void restore_modes(struct vt *v) {
    for (unsigned i=0;i<v->nparam;i++)
        for (unsigned k=0;k<v->nxtsave;k++) if (v->xtsave[k][0]==v->params[i]) set_dec_mode(v,v->params[i],v->xtsave[k][1]);
    v->revision++;
}
/* DECSTR: the modes and the pen back to their power-on state; the screen and
 * the cursor's place stay. */
static void soft_reset(struct vt *v) {
    struct vt_screen *s=vt_active(v);
    v->cursor_visible=v->autowrap=true;
    v->insert_mode=v->application_cursor=v->keypad_application=false;
    s->cursor.origin=false;s->top=0;s->bottom=v->rows-1;
    memset(&s->cursor.pen,0,sizeof s->cursor.pen);
    s->cursor.charset=0;memset(s->cursor.graphics,0,sizeof s->cursor.graphics);
    s->cursor.wrap=false;
    s->saved=s->cursor;s->saved.x=s->saved.y=0;
    v->pen_dirty=true;v->revision++;
}

/* --- CSI -------------------------------------------------------------------------- */

static void tab_forward(struct vt *v,int n) {
    struct vt_cursor *c=&vt_active(v)->cursor;
    while (n-- > 0 && c->x<v->cols-1) {c->x++;while (c->x<v->cols-1 && !v->tabs[c->x]) c->x++;}
    c->wrap=false;
}
static void tab_back(struct vt *v,int n) {
    struct vt_cursor *c=&vt_active(v)->cursor;
    while (n-- > 0 && c->x>0) {c->x--;while (c->x>0 && !v->tabs[c->x]) c->x--;}
    c->wrap=false;
}
static void window_op(struct vt *v) {
    int op=param0(v,0);
    int cell_w=v->cols?v->pixel_width/v->cols:0,cell_h=v->rows?v->pixel_height/v->rows:0;
    switch (op) {
    case 11: vt_reply(v,"\033[1t");break;                                 /* not iconified */
    case 13: vt_reply(v,"\033[3;0;0t");break;                             /* position */
    case 14: replyf(v,"\033[4;%d;%dt",v->pixel_height,v->pixel_width);break;
    case 16: replyf(v,"\033[6;%d;%dt",cell_h,cell_w);break;
    case 18: case 19: replyf(v,"\033[%d;%d;%dt",op==18?8:9,v->rows,v->cols);break;
    case 22:            /* push the title (0 and 2 name it; the icon label has no stack) */
        if (param0(v,1)!=1) {
            if (v->title_depth==8) {memmove(v->title_stack[0],v->title_stack[1],sizeof v->title_stack[0]*7);v->title_depth=7;}
            memcpy(v->title_stack[v->title_depth++],v->title,sizeof v->title);
        }
        break;
    case 23:
        if (param0(v,1)!=1 && v->title_depth) {memcpy(v->title,v->title_stack[--v->title_depth],sizeof v->title);v->revision++;}
        break;
    default: break;     /* moving, raising, resizing the window, reporting its title: not a program's to do */
    }
}
static void csi_dispatch(struct vt *v,unsigned char final) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;
    int n=param(v,0,1),p=param0(v,0);
    char pm=v->private_marker,im=v->nintermediate?v->intermediate[0]:0;
    if (v->nintermediate>1) return;
    if (pm=='?') {
        if (!im) switch (final) {
        case 'h': modes(v,true);return;
        case 'l': modes(v,false);return;
        case 's': save_modes(v);return;
        case 'r': restore_modes(v);return;
        case 'J': case 'K': break;                  /* DECSED/DECSEL: no protected cells, so ED/EL */
        case 'n':
            if (p==6) replyf(v,"\033[?%d;%dR",c->y+1-(c->origin?s->top:0),c->x+1);
            else if (p==15) vt_reply(v,"\033[?13n");                 /* no printer */
            else if (p==25) vt_reply(v,"\033[?21n");                 /* UDKs locked */
            else if (p==26) vt_reply(v,"\033[?27;1;0;0n");           /* North American keyboard, ready */
            return;
        default: return;
        }
        else if (im=='$' && final=='p') {replyf(v,"\033[?%d;%d$y",p,dec_mode(v,p));return;}
        else return;
    } else if (pm=='>') {
        if (!im && final=='c') vt_reply(v,"\033[>1;10;0c");                  /* DA2: a VT220 */
        else if (!im && final=='q') vt_reply(v,"\033P>|" VERSION "\033\\");  /* XTVERSION */
        return;           /* XTMODKEYS and the like: keys stay xterm's defaults */
    } else if (pm=='=') {
        if (!im && final=='c') vt_reply(v,"\033P!|00000000\033\\");           /* DA3 */
        return;
    } else if (pm) return;                     /* `<`: the kitty keyboard protocol, unsupported */
    if (im) {
        if (im==' ' && final=='q') {v->cursor_style=(uint8_t)vt_clamp(p,0,6);v->revision++;}
        else if (im=='!' && final=='p') soft_reset(v);
        else if (im=='$' && final=='p') {
            int state=p==4?(v->insert_mode?1:2):p==20?(v->newline_mode?1:2):p==2?4:p==12?3:0;
            replyf(v,"\033[%d;%d$y",p,state);
        }
        return;
    }
    switch (final) {
    case 'A': vt_cursor_to(v,c->x,c->y>=s->top?(c->y-n<s->top?s->top:c->y-n):c->y-n);break;
    case 'B': case 'e': vt_cursor_to(v,c->x,c->y<=s->bottom?(c->y+n>s->bottom?s->bottom:c->y+n):c->y+n);break;
    case 'C': case 'a': vt_cursor_to(v,c->x+n,c->y);break;
    case 'D': vt_cursor_to(v,c->x-n,c->y);break;
    case 'E': vt_cursor_to(v,0,c->y<=s->bottom?(c->y+n>s->bottom?s->bottom:c->y+n):c->y+n);break;
    case 'F': vt_cursor_to(v,0,c->y>=s->top?(c->y-n<s->top?s->top:c->y-n):c->y-n);break;
    case 'G': case '`': vt_cursor_to(v,n-1,c->y);break;
    case 'H': case 'f': vt_cursor_to(v,param(v,1,1)-1,n-1+(c->origin?s->top:0));break;
    case 'd': vt_cursor_to(v,c->x,n-1+(c->origin?s->top:0));break;
    case 'I': tab_forward(v,n);break;
    case 'Z': tab_back(v,n);break;
    case 'J':
        if (p==0) {vt_erase(v,c->y,c->x,v->cols);for (int y=c->y+1;y<v->rows;y++) vt_erase(v,y,0,v->cols);}
        else if (p==1) {for (int y=0;y<c->y;y++) vt_erase(v,y,0,v->cols);vt_erase(v,c->y,0,c->x+1);}
        else if (p==2) for (int y=0;y<v->rows;y++) vt_erase(v,y,0,v->cols);
        else if (p==3) vt_clear_history(v);
        c->wrap=false;break;
    case 'K':
        if (p==0) vt_erase(v,c->y,c->x,v->cols);
        else if (p==1) vt_erase(v,c->y,0,c->x+1);
        else if (p==2) vt_erase(v,c->y,0,v->cols);
        c->wrap=false;break;
    case 'X': vt_erase(v,c->y,c->x,c->x+n);c->wrap=false;break;
    case '@': vt_insert_cells(v,n);break;
    case 'P': vt_delete_cells(v,n);break;
    case 'L': case 'M':
        if (c->y>=s->top && c->y<=s->bottom) {vt_scroll(v,c->y,s->bottom,n,final=='M');c->x=0;}
        c->wrap=false;break;
    case 'S': vt_scroll(v,s->top,s->bottom,n,true);c->wrap=false;break;
    case 'T': if (v->nparam<=1) {vt_scroll(v,s->top,s->bottom,n,false);c->wrap=false;} break;
    case 'b':                                   /* REP */
        if (v->last_cp) {uint32_t cp=v->last_cp;for (int k=vt_clamp(n,1,65535);k--;) vt_print(v,cp);}
        break;
    case 'c': if (p==0) vt_reply(v,"\033[?62;22c");break;          /* DA1: VT220, ANSI colour */
    case 'g': if (p==0) v->tabs[c->x]=false; else if (p==3) memset(v->tabs,0,VT_MAX_COLS*sizeof *v->tabs);break;
    case 'h': modes(v,true);return;
    case 'l': modes(v,false);return;
    case 'm': sgr(v);return;
    case 'n':
        if (p==5) vt_reply(v,"\033[0n");
        else if (p==6) replyf(v,"\033[%d;%dR",c->y+1-(c->origin?s->top:0),c->x+1);
        break;
    case 'r': {
        int top=n-1,bottom=param(v,1,v->rows)-1;
        if (bottom>=v->rows) bottom=v->rows-1;
        if (top<bottom) {s->top=top;s->bottom=bottom;vt_cursor_to(v,0,c->origin?top:0);}
        break;
    }
    case 's': vt_save_cursor(v);break;
    case 'u': vt_restore_cursor(v);break;
    case 't': window_op(v);break;
    default: break;
    }
    v->revision++;
}

/* --- ESC ---------------------------------------------------------------------------- */

static void esc_dispatch(struct vt *v,unsigned char final) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;
    if (v->nintermediate) {
        char im=v->intermediate[0];
        if (im>='(' && im<='+') c->graphics[im-'(']=final=='0';    /* designate G0-G3 */
        else if (im=='#' && final=='8') vt_fill_e(v);
        return;                               /* ESC % G, ESC SP F, double-size lines: nothing to do */
    }
    switch (final) {
    case '7': vt_save_cursor(v);break;
    case '8': vt_restore_cursor(v);break;
    case 'D': vt_index(v,false);break;
    case 'E': c->x=0;vt_index(v,false);break;
    case 'M': vt_index(v,true);break;
    case 'H': v->tabs[c->x]=true;break;
    case 'c': vt_reset(v);break;
    case '=': v->keypad_application=true;break;
    case '>': v->keypad_application=false;break;
    case 'n': c->charset=2;break;             /* LS2 */
    case 'o': c->charset=3;break;             /* LS3 */
    case 'Z': vt_reply(v,"\033[?62;22c");break;
    default: break;
    }
    v->revision++;
}

/* --- strings ----------------------------------------------------------------------------- */

static int hexval(char ch) {
    return ch>='0' && ch<='9'?ch-'0':ch>='a' && ch<='f'?ch-'a'+10:ch>='A' && ch<='F'?ch-'A'+10:-1;
}
/* An X11 colour: rgb:R/G/B (1-4 hex digits a part) or #RGB .. #RRRRGGGGBBBB. */
static bool parse_color(const char *s,uint32_t *out) {
    unsigned part[3];
    if (!strncmp(s,"rgb:",4)) {
        s+=4;
        for (int k=0;k<3;k++) {
            unsigned val=0;int digits=0;
            while (hexval(*s)>=0 && digits<4) {val=val*16+(unsigned)hexval(*s++);digits++;}
            if (!digits || (k<2 && *s++!='/')) return false;
            unsigned max=(1u<<(4*digits))-1;
            part[k]=(val*255+max/2)/max;
        }
        if (*s) return false;
    } else if (*s=='#') {
        size_t len=strlen(++s);
        if (len%3 || !len || len>12) return false;
        int digits=(int)len/3;
        for (int k=0;k<3;k++) {
            unsigned val=0;
            for (int d=0;d<digits;d++) {int h=hexval(s[k*digits+d]);if (h<0) return false;val=val*16+(unsigned)h;}
            part[k]=digits==1?val*17:val>>(4*(digits-2));
        }
    } else return false;
    *out=(part[0]<<16)|(part[1]<<8)|part[2];
    return true;
}
/* A string a program sent, cut to fit -- never in the middle of a UTF-8
 * character, so a long title still ends in text the renderer can draw. */
static void keep_text(char *dst,size_t cap,const char *src) {
    size_t n=strlen(src);
    if (n>=cap) {n=cap-1;while (n && ((unsigned char)src[n]&0xc0)==0x80) n--;}
    memcpy(dst,src,n);dst[n]=0;
}
static void color_reply(struct vt *v,const char *head,uint32_t rgb,bool bel) {
    unsigned r=(rgb>>16)&255,g=(rgb>>8)&255,b=rgb&255;
    replyf(v,"\033]%srgb:%02x%02x/%02x%02x/%02x%02x%s",head,r,r,g,g,b,b,bel?"\a":"\033\\");
}
static void osc_palette(struct vt *v,char *args,bool bel) {
    while (args && *args) {
        char *spec=strchr(args,';');
        if (!spec) return;
        *spec++=0;
        char *next=strchr(spec,';');
        if (next) *next++=0;
        int i=atoi(args);
        if (i>=0 && i<256) {
            if (!strcmp(spec,"?")) {char head[16];snprintf(head,sizeof head,"4;%d;",i);color_reply(v,head,vt_rgb(v,VT_COLOR_INDEX(i),true),bel);}
            else {
                uint32_t rgb;
                if (parse_color(spec,&rgb)) {v->over.color[i]=rgb;v->over_set[i>>3]|=(uint8_t)(1u<<(i&7));v->revision++;}
            }
        }
        args=next;
    }
}
static void osc_dynamic(struct vt *v,int which,char *args,bool bel) {
    /* OSC 10;a;b;c sets 10, 11, 12 in turn */
    for (char *spec=args;spec && which<=12;which++) {
        char *next=strchr(spec,';');
        if (next) *next++=0;
        uint32_t current=which==10?vt_rgb(v,VT_COLOR_DEFAULT,true):which==11?vt_rgb(v,VT_COLOR_DEFAULT,false):vt_cursor_rgb(v);
        if (!strcmp(spec,"?")) {char head[8];snprintf(head,sizeof head,"%d;",which);color_reply(v,head,current,bel);}
        else {
            uint32_t rgb;
            if (parse_color(spec,&rgb)) {
                if (which==10) {v->over.fg=rgb;v->over_fg=true;}
                else if (which==11) {v->over.bg=rgb;v->over_bg=true;}
                else {v->over.cursor=rgb;v->over_cursor=true;}
                v->revision++;
            }
        }
        spec=next;
    }
}
static int base64_value(char ch) {
    return ch>='A' && ch<='Z'?ch-'A':ch>='a' && ch<='z'?ch-'a'+26:ch>='0' && ch<='9'?ch-'0'+52:ch=='+'?62:ch=='/'?63:-1;
}
static void osc_clipboard(struct vt *v,char *args) {
    char *data=strchr(args,';');
    if (!data || !v->allow_clipboard_write) return;    /* writing is opt-in; reading never answers */
    data++;
    if (!strcmp(data,"?")) return;
    size_t n=0;unsigned acc=0;int bits=0;
    for (const char *p=data;*p && *p!='=';p++) {
        int val=base64_value(*p);
        if (val<0) return;
        acc=(acc<<6)|(unsigned)val;bits+=6;
        if (bits>=8) {bits-=8;if (n<sizeof v->clipboard-1) v->clipboard[n++]=(char)((acc>>bits)&255);}
    }
    v->clipboard[n]=0;v->clipboard_len=n;v->clipboard_pending=true;
}
/* file://host/path, percent-decoded: the path. */
static void osc_directory(struct vt *v,const char *url) {
    if (strncmp(url,"file://",7)) return;
    const char *path=strchr(url+7,'/');
    if (!path) return;
    size_t n=0;
    for (const char *p=path;*p && n<sizeof v->directory-1;p++) {
        if (*p=='%' && hexval(p[1])>=0 && hexval(p[2])>=0) {
            char ch=(char)(hexval(p[1])*16+hexval(p[2]));p+=2;
            v->directory[n++]=(unsigned char)ch<32?'?':ch;
        } else v->directory[n++]=*p;
    }
    v->directory[n]=0;v->revision++;
}
static void osc_hyperlink(struct vt *v,char *args) {
    struct vt_style *pen=&vt_active(v)->cursor.pen;
    char *uri=strchr(args,';');
    if (!uri) return;
    pen->link=uri[1]?vt_intern_link(v,args):0;        /* "params;uri": an id= keeps two links apart */
    v->pen_dirty=true;
}
static void osc_shell(struct vt *v,const char *s) {
    /* OSC 133: A prompt start (fields: edit=shell, cwd= last, running to the
     * end, so a directory may hold ';'); B input start; C output start; D;n
     * the command finished with status n; R (EmbLink): back to the prompt's
     * first row, before the shell redraws it. */
    char kind=s[0];
    if (!kind || (s[1] && s[1]!=';')) return;
    if (kind=='A') {
        v->edit_shell=false;v->directory[0]=0;
        for (const char *f=s+1;*f==';';) {
            f++;
            if (!strncmp(f,"cwd=",4)) {keep_text(v->directory,sizeof v->directory,f+4);break;}
            const char *end=strchr(f,';');size_t n=end?(size_t)(end-f):strlen(f);
            if (n==10 && !strncmp(f,"edit=shell",10)) v->edit_shell=true;
            f+=n;
        }
        v->prompt_row=(int64_t)v->scrolls+vt_active(v)->cursor.y;
        v->prompt_serial++;v->shell_ready=true;
    } else if (kind=='C') {v->prompt_serial++;v->shell_ready=false;}
    else if (kind=='D') v->last_status=s[1]==';'?atoi(s+2):0;
    else if (kind=='R' && !v->alt && v->prompt_row>=0) {
        int64_t y=v->prompt_row-(int64_t)v->scrolls;
        struct vt_cursor *c=&v->primary.cursor;
        c->x=0;c->y=(int)(y<0?0:y>=v->rows?v->rows-1:y);c->wrap=false;
    }
    v->revision++;
}
static void osc_dispatch(struct vt *v,bool bel) {
    char *s=v->string;
    int code=0;
    while (*s>='0' && *s<='9') code=code*10+*s++-'0';
    if (*s==';') s++;
    else if (*s) return;
    switch (code) {
    case 0: case 1: case 2:
        if (code!=1) keep_text(v->title,sizeof v->title,s);
        if (code!=2) keep_text(v->icon,sizeof v->icon,s);
        v->revision++;break;
    case 4: osc_palette(v,s,bel);break;
    case 7: osc_directory(v,s);break;
    case 8: osc_hyperlink(v,s);break;
    case 10: case 11: case 12: osc_dynamic(v,code,s,bel);break;
    case 52: osc_clipboard(v,s);break;
    case 104:
        if (!*s) memset(v->over_set,0,sizeof v->over_set);
        else for (char *p=s;p;) {
            int i=atoi(p);
            if (i>=0 && i<256) v->over_set[i>>3]&=(uint8_t)~(1u<<(i&7));
            p=strchr(p,';');if (p) p++;
        }
        v->revision++;break;
    case 110: v->over_fg=false;v->revision++;break;
    case 111: v->over_bg=false;v->revision++;break;
    case 112: v->over_cursor=false;v->revision++;break;
    case 133: osc_shell(v,s);break;
    default: break;       /* notifications, pointer shapes, iTerm2's own: not here */
    }
}
/* XTGETTCAP: what terminfo would say, for programs that ask the terminal
 * itself rather than trust $TERM. */
static const char *tcap(const char *name) {
    static const char *const caps[][2]={
        {"TN","xterm-256color"},{"Co","256"},{"colors","256"},{"RGB","8/8/8"},
        {"Smulx","\033[4:%p1%dm"},{"Setulc","\033[58:2::%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%dm"},
        {"Ss","\033[%p1%d q"},{"Se","\033[2 q"},{"Sync","\033[?2026%?%p1%{1}%-%tl%eh%;"},
        {"Ms","\033]52;%p1%s;%p2%s\a"},{"kbs","\177"},{"bce",""},{"Tc",""},
    };
    for (size_t i=0;i<sizeof caps/sizeof *caps;i++) if (!strcmp(caps[i][0],name)) return caps[i][1];
    return NULL;
}
static void dcs_dispatch(struct vt *v) {
    const char *s=v->string;
    if (!strncmp(s,"$q",2)) {               /* DECRQSS */
        const char *what=s+2;struct vt_screen *sc=vt_active(v);
        char body[256];
        if (!strcmp(what,"m")) {pen_sgr(&sc->cursor.pen,body,sizeof body);replyf(v,"\033P1$r%sm\033\\",body);}
        else if (!strcmp(what,"r")) replyf(v,"\033P1$r%d;%dr\033\\",sc->top+1,sc->bottom+1);
        else if (!strcmp(what," q")) replyf(v,"\033P1$r%d q\033\\",v->cursor_style);
        else if (!strcmp(what,"\"p")) vt_reply(v,"\033P1$r62;1\"p\033\\");
        else if (!strcmp(what,"\"q")) vt_reply(v,"\033P1$r0\"q\033\\");
        else vt_reply(v,"\033P0$r\033\\");
    } else if (!strncmp(s,"+q",2)) {        /* XTGETTCAP: hex names, ';' between */
        for (const char *p=s+2;*p;) {
            const char *end=strchr(p,';');size_t len=end?(size_t)(end-p):strlen(p);
            char name[64];size_t n=0;bool ok=len%2==0 && len/2<sizeof name;
            for (size_t k=0;ok && k<len;k+=2) {
                int hi=hexval(p[k]),lo=hexval(p[k+1]);
                if (hi<0 || lo<0) ok=false; else name[n++]=(char)(hi*16+lo);
            }
            name[n]=0;
            const char *value=ok?tcap(name):NULL;
            if (value) {
                char out[400];size_t o=0;
                for (const char *q=value;*q && o+2<sizeof out;q++) o+=(size_t)snprintf(out+o,sizeof out-o,"%02X",(unsigned char)*q);
                out[o]=0;
                replyf(v,"\033P1+r%.*s%s%s\033\\",(int)len,p,*value?"=":"",out);
            } else replyf(v,"\033P0+r%.*s\033\\",(int)(len<128?len:128),p);
            p+=len;if (*p==';') p++;
        }
    }
}
static void string_end(struct vt *v,bool bel) {
    v->string[v->string_length]=0;
    if (!v->overflow) {
        if (v->string_kind==STR_OSC) osc_dispatch(v,bel);
        else if (v->string_kind==STR_DCS) dcs_dispatch(v);
    }
    v->state=GROUND;v->string_escape=false;
}

/* --- the state machine ----------------------------------------------------------------------- */

static void clear_sequence(struct vt *v) {
    v->nparam=0;v->nintermediate=0;v->private_marker=0;v->overflow=false;
    v->params[0]=-1;v->subparam[0]=0;
}
static void execute(struct vt *v,unsigned char b) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;
    switch (b) {
    case 7: v->bells++;break;
    case 8: if (c->x>0) c->x--;c->wrap=false;break;
    case 9: tab_forward(v,1);break;
    case 10: case 11: case 12:
        s->rows[c->y]->wrapped=false;
        vt_index(v,false);
        if (v->newline_mode) c->x=0;
        break;
    case 13: c->x=0;c->wrap=false;break;
    case 14: c->charset=1;break;
    case 15: c->charset=0;break;
    default: return;
    }
    v->revision++;
}
static void collect_param(struct vt *v,unsigned char b) {
    if (!v->nparam) {v->nparam=1;v->params[0]=-1;v->subparam[0]=0;}
    if (b==';' || b==':') {
        if (v->nparam==32) {v->overflow=true;return;}
        v->params[v->nparam]=-1;v->subparam[v->nparam]=b==':';v->nparam++;
        return;
    }
    int *p=&v->params[v->nparam-1];
    if (*p<0) *p=0;
    *p=*p>6553?65535:*p*10+(b-'0');
    if (*p>65535) *p=65535;
}
static void utf8_byte(struct vt *v,unsigned char b);
static void ground_print(struct vt *v,unsigned char b) {
    if (b<0x80) {vt_print(v,b);return;}
    utf8_byte(v,b);
}
static void utf8_byte(struct vt *v,unsigned char b) {
    if (v->utf8_left) {
        if ((b&0xc0)!=0x80) {v->utf8_left=0;vt_print(v,0xfffd);ground_print(v,b);return;}
        v->utf8=(v->utf8<<6)|(b&63);
        if (--v->utf8_left==0) {
            uint32_t cp=v->utf8;
            vt_print(v,cp<v->utf8_min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)?0xfffd:cp);
        }
    } else if (b>=0xc2 && b<=0xdf) {v->utf8=b&31;v->utf8_min=0x80;v->utf8_left=1;}
    else if (b>=0xe0 && b<=0xef) {v->utf8=b&15;v->utf8_min=0x800;v->utf8_left=2;}
    else if (b>=0xf0 && b<=0xf4) {v->utf8=b&7;v->utf8_min=0x10000;v->utf8_left=3;}
    else vt_print(v,0xfffd);
}
static void byte(struct vt *v,unsigned char b) {
    /* in a string, only its end and the cancels count */
    if (v->state==STRING) {
        if (v->string_escape) {
            v->string_escape=false;
            if (b=='\\') {string_end(v,false);return;}
            /* ESC then something else: the string ends unfinished; that ESC starts anew */
            v->state=ESCAPE;clear_sequence(v);
            byte(v,b);
            return;
        }
        if (b==0x18 || b==0x1a) {v->state=GROUND;return;}
        if (b==27) {v->string_escape=true;return;}
        if (b==7 && v->string_kind==STR_OSC) {string_end(v,true);return;}
        if (b<32 || b==127) return;
        if (v->string_length<sizeof v->string-1) v->string[v->string_length++]=(char)b;
        else v->overflow=true;
        return;
    }
    if (b==0x18 || b==0x1a) {
        if (v->utf8_left) {v->utf8_left=0;vt_print(v,0xfffd);}
        if (b==0x1a && v->state!=GROUND) vt_print(v,0xfffd);
        v->state=GROUND;return;
    }
    if (b==27) {
        if (v->utf8_left) {v->utf8_left=0;vt_print(v,0xfffd);}
        v->state=ESCAPE;clear_sequence(v);return;
    }
    if (b<32 || b==127) {
        if (v->utf8_left) {v->utf8_left=0;vt_print(v,0xfffd);}
        if (b!=127) {execute(v,b);if (b!=7) v->last_cp=0;}
        return;
    }
    switch (v->state) {
    case GROUND: ground_print(v,b);return;
    case ESCAPE:
        if (b>=0x20 && b<=0x2f) {v->intermediate[0]=(char)b;v->nintermediate=1;v->state=ESCAPE_INTERMEDIATE;return;}
        v->state=GROUND;
        switch (b) {
        case '[': v->state=CSI;clear_sequence(v);return;
        case ']': v->state=STRING;v->string_kind=STR_OSC;break;
        case 'P': v->state=STRING;v->string_kind=STR_DCS;break;
        case 'X': case '^': case '_': v->state=STRING;v->string_kind=STR_OTHER;break;
        default: if (b>=0x30 && b<=0x7e) {esc_dispatch(v,b);v->last_cp=0;} return;
        }
        v->string_length=0;v->string_escape=false;v->overflow=false;
        return;
    case ESCAPE_INTERMEDIATE:
        if (b>=0x20 && b<=0x2f) {if (v->nintermediate<2) v->intermediate[v->nintermediate++]=(char)b; return;}
        v->state=GROUND;
        if (b>=0x30 && b<=0x7e) esc_dispatch(v,b);
        return;
    case CSI:
        if ((b>='0' && b<='9') || b==';' || b==':') {
            if (v->nintermediate) {v->state=CSI_IGNORE;return;}
            collect_param(v,b);return;
        }
        if (b>='<' && b<='?') {
            if (v->nparam || v->nintermediate || v->private_marker) {v->state=CSI_IGNORE;return;}
            v->private_marker=(char)b;return;
        }
        if (b>=0x20 && b<=0x2f) {
            if (v->nintermediate<2) v->intermediate[v->nintermediate++]=(char)b; else v->overflow=true;
            return;
        }
        v->state=GROUND;
        if (b>=0x40 && b<=0x7e) {
            if (v->overflow) return;
            if (!v->nparam) {v->params[0]=-1;v->subparam[0]=0;}
            csi_dispatch(v,b);
            if (b!='b') v->last_cp=0;
        }
        return;
    case CSI_IGNORE:
        if (b>=0x40 && b<=0x7e) v->state=GROUND;
        return;
    default: v->state=GROUND;return;
    }
}
void vt_feed(struct vt *v,const void *data,size_t length) {
    const unsigned char *p=data;
    for (size_t i=0;i<length;i++) {
        /* the common case, plain ASCII text, goes straight to the grid */
        if (v->state==GROUND && !v->utf8_left && p[i]>=0x20 && p[i]<0x7f) {vt_print(v,p[i]);continue;}
        byte(v,p[i]);
    }
}
