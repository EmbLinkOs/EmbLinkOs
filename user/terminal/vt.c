#include "vt.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int clamp(int x,int lo,int hi) { return x<lo?lo:x>hi?hi:x; }
static struct vt_line *line_at(struct vt_screen *s,int row) { return &s->lines[s->order[row]]; }
struct vt_screen *vt_active(struct vt *v) { return v->alt?&v->alternate:&v->primary; }
static struct vt_cell blank(const struct vt_cursor *c) {
    struct vt_cell b={0}; b.rune=' '; b.width=1; b.fg=c->fg; b.bg=c->bg; b.flags=c->flags; return b;
}
static void clear_line(struct vt_line *l,const struct vt_cursor *c) {
    struct vt_cell b=blank(c);
    for (int x=0;x<VT_COLS;x++) l->cells[x]=b;
    l->used=0; l->wrapped=l->wrap_pad=false;
}
static void init_screen(struct vt_screen *s,int rows) {
    memset(s,0,sizeof *s); s->cursor.fg=s->cursor.bg=VT_DEFAULT;
    s->saved=s->cursor; s->bottom=rows-1;
    for (unsigned i=0;i<VT_ROWS;i++) { s->order[i]=i; clear_line(&s->lines[i],&s->cursor); }
}
void vt_init(struct vt *v,int cols,int rows,vt_reply_fn reply,void *context) {
    memset(v,0,sizeof *v); v->cols=clamp(cols,2,VT_COLS); v->rows=clamp(rows,2,VT_ROWS);
    init_screen(&v->primary,v->rows); init_screen(&v->alternate,v->rows);
    v->autowrap=v->cursor_visible=v->alt_sends_escape=true; v->reply=reply; v->reply_context=context;
    for (int i=0;i<VT_COLS;i++) v->tabs[i]=i && i%8==0;
    v->revision=1;
}
void vt_reset(struct vt *v) {
    int cols=v->cols,rows=v->rows; vt_reply_fn reply=v->reply; void *ctx=v->reply_context;
    bool bs=v->backspace_sends_bs,meta=v->alt_sends_escape;
    uint64_t revision=v->revision; vt_init(v,cols,rows,reply,ctx); v->revision=revision+1;
    v->backspace_sends_bs=bs;v->alt_sends_escape=meta;
}
uint32_t vt_color(unsigned i) {
    static const uint32_t colors[16]={0x30343b,0xcf666c,0x73bb85,0xdbbf70,0x789fdb,0xb98bcc,0x6abec5,0xd9dfe7,
        0x81858e,0xf58b91,0x91dba0,0xf0d988,0x99bdf4,0xd4aaf1,0x8bdde2,0xf5f7fa};
    if (i<16) return colors[i];
    if (i<232) { i-=16; unsigned r=i/36,g=(i/6)%6,b=i%6; return ((r?55+r*40:0)<<16)|((g?55+g*40:0)<<8)|(b?55+b*40:0); }
    unsigned g=8+(i<256?i-232:23)*10; return (g<<16)|(g<<8)|g;
}
int vt_width(uint32_t c) {
    if (!c || c<32 || (c>=0x7f && c<0xa0)) return -1;
    if ((c>=0x300 && c<=0x36f) || (c>=0x1ab0 && c<=0x1aff) || (c>=0x1dc0 && c<=0x1dff) ||
        (c>=0x20d0 && c<=0x20ff) || (c>=0xfe00 && c<=0xfe0f) || (c>=0xfe20 && c<=0xfe2f) || c==0x200d ||
        (c>=0xe0100 && c<=0xe01ef)) return 0;
    if ((c>=0x1100 && c<=0x115f) || c==0x2329 || c==0x232a || (c>=0x2e80 && c<=0xa4cf && c!=0x303f) ||
        (c>=0xac00 && c<=0xd7a3) || (c>=0xf900 && c<=0xfaff) || (c>=0xfe10 && c<=0xfe19) ||
        (c>=0xfe30 && c<=0xfe6f) || (c>=0xff00 && c<=0xff60) || (c>=0xffe0 && c<=0xffe6) ||
        (c>=0x1f300 && c<=0x1faff) || (c>=0x20000 && c<=0x3fffd)) return 2;
    return 1;
}
unsigned vt_encode(uint32_t c,char out[4]) {
    if (c<0x80) {out[0]=(char)c;return 1;}
    if (c<0x800) {out[0]=(char)(0xc0|(c>>6));out[1]=(char)(0x80|(c&63));return 2;}
    if (c<0x10000) {out[0]=(char)(0xe0|(c>>12));out[1]=(char)(0x80|((c>>6)&63));out[2]=(char)(0x80|(c&63));return 3;}
    out[0]=(char)(0xf0|(c>>18));out[1]=(char)(0x80|((c>>12)&63));out[2]=(char)(0x80|((c>>6)&63));out[3]=(char)(0x80|(c&63));return 4;
}
static void remember(struct vt *v,const struct vt_line *l) {
    v->history[v->history_head]=*l;
    v->history_head=(v->history_head+1)%VT_HISTORY;
    if (v->history_count<VT_HISTORY) v->history_count++;
    v->scrolls++;
}
static void scroll(struct vt *v,int top,int bottom,int count,bool up) {
    struct vt_screen *s=vt_active(v); count=clamp(count,1,bottom-top+1);
    while (count--) {
        unsigned slot=s->order[up?top:bottom];
        if (up && !v->alt && top==0 && bottom==v->rows-1) remember(v,&s->lines[slot]);
        if (up) memmove(s->order+top,s->order+top+1,(size_t)(bottom-top)*sizeof *s->order);
        else memmove(s->order+top+1,s->order+top,(size_t)(bottom-top)*sizeof *s->order);
        s->order[up?bottom:top]=slot; clear_line(&s->lines[slot],&s->cursor);
    }
    v->revision++;
}
static void index_line(struct vt *v,bool reverse) {
    struct vt_screen *s=vt_active(v); struct vt_cursor *c=&s->cursor; c->wrap=false;
    if (reverse) {
        if (c->y==s->top) scroll(v,s->top,s->bottom,1,false); else if (c->y>0) c->y--;
    } else {
        if (c->y==s->bottom) scroll(v,s->top,s->bottom,1,true); else if (c->y<v->rows-1) c->y++;
    }
    v->revision++;
}
static void erase_cell(struct vt *v,struct vt_line *l,int x) {
    struct vt_cell b=blank(&vt_active(v)->cursor);
    if (!l->cells[x].width && x>0) l->cells[x-1]=b;
    if (l->cells[x].width==2 && x+1<v->cols) l->cells[x+1]=b;
    l->cells[x]=b;
}
static void normalize_to(struct vt_line *l,int cols,const struct vt_cursor *cursor) {
    struct vt_cell b=blank(cursor);
    for (int x=0;x<cols;x++) {
        if (l->cells[x].width==2) {
            if (x+1==cols) l->cells[x]=b;
            else { l->cells[x+1]=b; l->cells[++x].width=0; }
        } else if (!l->cells[x].width) l->cells[x]=b;
    }
    l->used=0;
    for (int x=0;x<cols;x++) if (l->cells[x].rune!=' ' || l->cells[x].bg!=VT_DEFAULT || l->cells[x].flags) l->used=(unsigned)(x+(l->cells[x].width==2?2:1));
}
static void normalize(struct vt *v,struct vt_line *l) {normalize_to(l,v->cols,&vt_active(v)->cursor);}
static void erase_range(struct vt *v,int row,int start,int end) {
    struct vt_line *l=line_at(vt_active(v),row);
    for (int x=start;x<end;x++) erase_cell(v,l,x);
    normalize(v,l); l->wrapped=l->wrap_pad=false; v->revision++;
}
static uint32_t graphics(uint32_t c) {
    static const uint32_t map[31]={0x25c6,0x2592,0x2409,0x240c,0x240d,0x240a,0xb0,0xb1,0x2424,0x240b,
        0x2518,0x2510,0x250c,0x2514,0x253c,0x23ba,0x23bb,0x2500,0x23bc,0x23bd,0x251c,0x2524,0x2534,0x252c,0x2502,0x2264,0x2265,0x3c0,0x2260,0xa3,0xb7};
    return c>=0x60 && c<=0x7e?map[c-0x60]:c;
}
static void put_cell(struct vt *v,struct vt_cell cell) {
    struct vt_screen *s=vt_active(v); struct vt_cursor *c=&s->cursor;
    int width=cell.width;
    if (width==0) {
        int y=c->y,x=c->x-(c->wrap?0:1);
        if (x<0 && y>0 && line_at(s,y-1)->wrapped) { y--; x=v->cols-1; }
        if (x<0) return;
        struct vt_cell *prev=&line_at(s,y)->cells[x];
        if (!prev->width && x>0) prev--;
        if (!prev->combining[0]) prev->combining[0]=cell.rune;
        else if (!prev->combining[1]) prev->combining[1]=cell.rune;
        v->revision++; return;
    }
    if (c->wrap && v->autowrap) {
        line_at(s,c->y)->wrapped=true; line_at(s,c->y)->wrap_pad=false; index_line(v,false); c->x=0;
    }
    c->wrap=false;
    if (width==2 && c->x==v->cols-1) {
        if (v->autowrap) { erase_cell(v,line_at(s,c->y),c->x); line_at(s,c->y)->wrapped=true; line_at(s,c->y)->wrap_pad=true; index_line(v,false); c->x=0; }
        else { cell.rune=0xfffd;cell.width=1;width=1; }
    }
    struct vt_line *l=line_at(s,c->y);
    erase_cell(v,l,c->x); if (width==2) erase_cell(v,l,c->x+1);
    if(c->x+width==v->cols)l->wrap_pad=false;
    l->cells[c->x]=cell;
    if (width==2) { l->cells[c->x+1]=blank(c); l->cells[c->x+1].width=0; }
    if (l->used<(unsigned)(c->x+width)) l->used=(unsigned)(c->x+width);
    c->x+=width;
    if (c->x>=v->cols) {c->x=v->cols-1;c->wrap=v->autowrap;}
    v->revision++;
}
static void printable(struct vt *v,uint32_t rune) {
    int w=vt_width(rune); if (w<0) return;
    struct vt_cursor *c=&vt_active(v)->cursor;
    struct vt_cell cell=blank(c); cell.rune=v->graphics[v->charset]?graphics(rune):rune; cell.width=(uint8_t)w;
    put_cell(v,cell);
}
static void cursor_to(struct vt *v,int x,int y) {
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;
    c->x=clamp(x,0,v->cols-1);c->y=clamp(y,c->origin?s->top:0,c->origin?s->bottom:v->rows-1);
    c->wrap=false; v->revision++;
}
static void reply(struct vt *v,const char *s) { if (v->reply) v->reply(v->reply_context,s,strlen(s)); }
static int param(struct vt *v,unsigned i,int fallback) { return i<v->nparam && v->params[i]>0?v->params[i]:fallback; }
static void save_cursor(struct vt *v) { struct vt_screen *s=vt_active(v);s->saved=s->cursor; }
static void restore_cursor(struct vt *v) {
    struct vt_screen *s=vt_active(v);s->cursor=s->saved;
    s->cursor.x=clamp(s->cursor.x,0,v->cols-1);s->cursor.y=clamp(s->cursor.y,0,v->rows-1);v->revision++;
}
static void alternate(struct vt *v,bool on,bool save,bool clear) {
    if (on && !v->alt) {
        if (save) save_cursor(v);
        v->alt_saved=save; if (clear) init_screen(&v->alternate,v->rows); v->alt=true;
    } else if (!on && v->alt) {
        v->alt=false; if (v->alt_saved) restore_cursor(v); v->alt_saved=false;
    }
    v->revision++;
}
static void sgr(struct vt *v) {
    struct vt_cursor *c=&vt_active(v)->cursor;
    for (unsigned i=0;i<v->nparam;i++) {
        int p=v->params[i]<0?0:v->params[i];
        if (!p) {c->fg=c->bg=VT_DEFAULT;c->flags=0;}
        else if (p==1)c->flags|=VT_BOLD; else if (p==2)c->flags|=VT_DIM;
        else if (p==3)c->flags|=VT_ITALIC; else if (p==4)c->flags|=VT_UNDERLINE;
        else if (p==7)c->flags|=VT_INVERSE; else if (p==8)c->flags|=VT_HIDDEN;
        else if (p==9)c->flags|=VT_STRIKE; else if (p==22)c->flags&=~(VT_BOLD|VT_DIM);
        else if (p==23)c->flags&=~VT_ITALIC; else if (p==24)c->flags&=~VT_UNDERLINE;
        else if (p==27)c->flags&=~VT_INVERSE; else if (p==28)c->flags&=~VT_HIDDEN;
        else if (p==29)c->flags&=~VT_STRIKE;
        else if (p>=30 && p<=37)c->fg=vt_color((unsigned)p-30);
        else if (p>=40 && p<=47)c->bg=vt_color((unsigned)p-40);
        else if (p>=90 && p<=97)c->fg=vt_color((unsigned)p-90+8);
        else if (p>=100 && p<=107)c->bg=vt_color((unsigned)p-100+8);
        else if (p==39)c->fg=VT_DEFAULT; else if (p==49)c->bg=VT_DEFAULT;
        else if (p==38 || p==48) {
            uint32_t rgb=VT_DEFAULT; bool valid=false;
            if (i+2<v->nparam && v->params[i+1]==5) {
                int index=v->params[i+2];valid=index>=0 && index<256;
                if (valid) rgb=vt_color((unsigned)index);
                i+=2;
            } else if (i+4<v->nparam && v->params[i+1]==2) {
                int r=v->params[i+2],g=v->params[i+3],b=v->params[i+4];
                valid=r>=0 && r<256 && g>=0 && g<256 && b>=0 && b<256;
                if (valid) rgb=((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
                i+=4;
            } else break; /* malformed colour operands must not become attributes */
            if (valid) { if (p==38)c->fg=rgb;else c->bg=rgb; }
        }
    }
}
static void mode(struct vt *v,bool on) {
    for (unsigned i=0;i<v->nparam;i++) {
        if (!v->private_csi) continue;
        int p=v->params[i];
        if (p==1)v->application_cursor=on;
        else if (p==6) {struct vt_screen *s=vt_active(v);s->addressed=true;s->cursor.origin=on;cursor_to(v,0,on?s->top:0);}
        else if (p==7) {v->autowrap=on;vt_active(v)->cursor.wrap=false;}
        else if (p==25)v->cursor_visible=on;
        else if (p==47)alternate(v,on,false,false);
        else if (p==1047)alternate(v,on,false,on);
        else if (p==1048) {if(on)save_cursor(v);else restore_cursor(v);}
        else if (p==1049)alternate(v,on,true,on);
        else if (p==2004)v->bracketed_paste=on;
    }
    v->revision++;
}
static void csi(struct vt *v,unsigned final) {
    if (v->bad_csi) return;
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;
    int n=param(v,0,1),p=v->params[0]<0?0:v->params[0];
    if (v->private_csi && final!='h' && final!='l' && final!='n' && final!='c') return;
    if (final=='m' && !v->intermediate) {sgr(v);return;}
    if (final=='h' || final=='l') {mode(v,final=='h');return;}
    if (final=='q' && v->intermediate==' ') {v->cursor_style=(uint8_t)clamp(p,0,6);v->revision++;return;}
    if (v->intermediate) return;
    switch (final) {
    case 'A':cursor_to(v,c->x,c->y-n);s->addressed=true;break;
    case 'B':case 'e':cursor_to(v,c->x,c->y+n);s->addressed=true;break;
    case 'C':case 'a':cursor_to(v,c->x+n,c->y);break;
    case 'D':cursor_to(v,c->x-n,c->y);break;
    case 'E':cursor_to(v,0,c->y+n);s->addressed=true;break;
    case 'F':cursor_to(v,0,c->y-n);s->addressed=true;break;
    case 'G':case '`':cursor_to(v,n-1,c->y);break;
    case 'H':case 'f':cursor_to(v,param(v,1,1)-1,n-1+(c->origin?s->top:0));s->addressed=true;break;
    case 'd':cursor_to(v,c->x,n-1+(c->origin?s->top:0));s->addressed=true;break;
    case 'J':
        if (p==0) {erase_range(v,c->y,c->x,v->cols);for(int y=c->y+1;y<v->rows;y++)erase_range(v,y,0,v->cols);}
        else if (p==1) {for(int y=0;y<c->y;y++)erase_range(v,y,0,v->cols);erase_range(v,c->y,0,c->x+1);}
        else if (p==2) {for(int y=0;y<v->rows;y++)erase_range(v,y,0,v->cols);}
        else if (p==3)vt_clear_history(v);
        c->wrap=false;break;
    case 'K':if(p==0)erase_range(v,c->y,c->x,v->cols);else if(p==1)erase_range(v,c->y,0,c->x+1);else if(p==2)erase_range(v,c->y,0,v->cols);c->wrap=false;break;
    case 'X':erase_range(v,c->y,c->x,clamp(c->x+n,0,v->cols));c->wrap=false;break;
    case '@':case 'P': {
        struct vt_line *l=line_at(s,c->y); n=clamp(n,1,v->cols-c->x);
        if (final=='@') {
            memmove(l->cells+c->x+n,l->cells+c->x,(size_t)(v->cols-c->x-n)*sizeof *l->cells);
            for(int x=c->x;x<c->x+n;x++)l->cells[x]=blank(c);
        } else {
            memmove(l->cells+c->x,l->cells+c->x+n,(size_t)(v->cols-c->x-n)*sizeof *l->cells);
            for(int x=v->cols-n;x<v->cols;x++)l->cells[x]=blank(c);
        }
        normalize(v,l);l->wrapped=l->wrap_pad=false;c->wrap=false;v->revision++;break;
    }
    case 'L':case 'M':if(c->y>=s->top && c->y<=s->bottom)scroll(v,c->y,s->bottom,n,final=='M');c->wrap=false;break;
    case 'S':scroll(v,s->top,s->bottom,n,true);c->wrap=false;break;
    case 'T':scroll(v,s->top,s->bottom,n,false);c->wrap=false;break;
    case 'r': {
        int top=n-1,bottom=param(v,1,v->rows)-1;
        if (top>=0 && top<bottom && bottom<v->rows) {s->top=top;s->bottom=bottom;cursor_to(v,0,c->origin?top:0);if(v->params[0]>=0)s->addressed=true;}
        break;
    }
    case 's':save_cursor(v);break;
    case 'u':restore_cursor(v);break;
    case 'g':if(p==0)v->tabs[c->x]=false;else if(p==3)memset(v->tabs,0,sizeof v->tabs);break;
    case 'I':while(n-- && c->x<v->cols-1) {c->x++;while(c->x<v->cols-1 && !v->tabs[c->x])c->x++;}c->wrap=false;break;
    case 'Z':while(n-- && c->x>0) {c->x--;while(c->x>0 && !v->tabs[c->x])c->x--;}c->wrap=false;break;
    case 'n':
        if (p==5 && !v->private_csi)reply(v,"\033[0n");
        else if (p==6) {char buf[40];snprintf(buf,sizeof buf,"\033[%s%d;%dR",v->private_csi?"?":"",c->y+1-(c->origin?s->top:0),c->x+1);reply(v,buf);}
        break;
    case 'c':if(p==0)reply(v,"\033[?1;2c");break;
    default:break;
    }
    v->revision++;
}
static void start_csi(struct vt *v) {
    v->state=2;v->nparam=1;v->private_csi=v->bad_csi=false;v->intermediate=0;
    for(unsigned i=0;i<16;i++)v->params[i]=-1;
}
static void finish_string(struct vt *v) {
    if (v->string_kind==']') {
        v->string[v->string_length]=0;
        if (!strncmp(v->string,"133;A;cwd=",10)) {
            size_t n=v->string_length-10;
            memcpy(v->directory,v->string+10,n);v->directory[n]=0;v->prompt_serial++;v->shell_ready=true;
        }
        if (!strcmp(v->string,"133;C")) {v->prompt_serial++;v->shell_ready=false;}
        if ((v->string[0]=='0' || v->string[0]=='2') && v->string[1]==';') {
            memcpy(v->title,v->string+2,v->string_length>=2?v->string_length-2:0);
            v->title[v->string_length>=2?v->string_length-2:0]=0;v->revision++;
        }
    }
    v->state=0;v->string_escape=0;
}
static void byte(struct vt *v,unsigned char b) {
again:
    if (v->state==3) {
        if (b==0x18 || b==0x1a) {v->state=0;return;}
        if (v->string_escape) {
            v->string_escape=0;if(b=='\\') {finish_string(v);return;}
        }
        if (b==27) {v->string_escape=1;return;}
        if (b==7 && v->string_kind==']') {finish_string(v);return;}
        if (v->string_length<sizeof v->string-1 && b>=32 && b!=127)v->string[v->string_length++]=(char)b;
        return;
    }
    if (b==0x18 || b==0x1a) {v->state=0;v->utf8_left=0;return;}
    if (b==27) {v->state=1;v->utf8_left=0;return;}
    if (v->state==4 || v->state==5) {v->graphics[v->state-4]=b=='0';v->state=0;return;}
    if (v->state==1) {
        v->state=0;
        switch(b) {
        case '[':start_csi(v);break;
        case ']':case 'P':case '^':case '_':v->state=3;v->string_kind=b;v->string_length=v->string_escape=0;break;
        case '7':save_cursor(v);break;
        case '8':restore_cursor(v);break;
        case 'D':index_line(v,false);break;
        case 'E':vt_active(v)->cursor.x=0;index_line(v,false);break;
        case 'M':index_line(v,true);break;
        case 'H':v->tabs[vt_active(v)->cursor.x]=true;break;
        case 'c':vt_reset(v);break;
        case '(':v->state=4;break;
        case ')':v->state=5;break;
        case '=':v->keypad_application=true;break;
        case '>':v->keypad_application=false;break;
        default:break;
        }
        return;
    }
    if (v->state==2 && b>=0x20) {
        if (b>='0' && b<='9' && !v->intermediate) {
            int *p=&v->params[v->nparam-1];if(*p<0)*p=0;
            if (*p>999) *p=9999; else *p=*p*10+b-'0';
        } else if (b==';' && !v->intermediate) {
            if(v->nparam<16)v->nparam++;else v->bad_csi=true;
        } else if (b=='?' && v->nparam==1 && v->params[0]<0 && !v->intermediate) v->private_csi=true;
        else if (b>=0x20 && b<=0x2f) {if(v->intermediate)v->bad_csi=true;v->intermediate=b;}
        else if (b>=0x40 && b<=0x7e) {csi(v,b);v->state=0;}
        else v->bad_csi=true;
        return;
    }
    struct vt_screen *s=vt_active(v);struct vt_cursor *c=&s->cursor;
    if (b<32 || b==127) {
        if(v->utf8_left) {v->utf8_left=0;printable(v,0xfffd);}
        if (b==8) {if(c->x>0)c->x--;c->wrap=false;}
        else if (b==9) {c->wrap=false;if(c->x<v->cols-1)c->x++;while(c->x<v->cols-1 && !v->tabs[c->x])c->x++;}
        else if (b==10 || b==11 || b==12) {line_at(s,c->y)->wrapped=false;index_line(v,false);}
        else if (b==13) {c->x=0;c->wrap=false;}
        else if (b==14)v->charset=1;else if(b==15)v->charset=0;
        v->revision++;return;
    }
    if (v->state==2) return; /* C0 controls are allowed inside CSI */
    if (v->utf8_left) {
        if ((b&0xc0)!=0x80) {v->utf8_left=0;printable(v,0xfffd);goto again;}
        v->utf8=(v->utf8<<6)|(b&63);
        if (--v->utf8_left==0) {
            uint32_t cp=v->utf8;
            printable(v,cp<v->utf8_min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)?0xfffd:cp);
        }
    } else if (b<128) printable(v,b);
    else if (b>=0xc2 && b<=0xdf) {v->utf8=b&31;v->utf8_min=0x80;v->utf8_left=1;}
    else if (b>=0xe0 && b<=0xef) {v->utf8=b&15;v->utf8_min=0x800;v->utf8_left=2;}
    else if (b>=0xf0 && b<=0xf4) {v->utf8=b&7;v->utf8_min=0x10000;v->utf8_left=3;}
    else printable(v,0xfffd);
}
void vt_feed(struct vt *v,const void *data,size_t length) {
    const unsigned char *p=data;for(size_t i=0;i<length;i++)byte(v,p[i]);
}
void vt_clear_history(struct vt *v) {v->history_head=v->history_count=0;v->revision++;}
void vt_prompt_room(struct vt *v,unsigned extra) {
    if (v->alt) return;
    extra=(unsigned)clamp((int)extra,0,v->rows-1);
    while (v->primary.cursor.y+(int)extra>=v->rows) {
        scroll(v,0,v->rows-1,1,true);v->primary.cursor.y--;
    }
}
const struct vt_cell *vt_view_cell(const struct vt *v,int row,int col,unsigned back) {
    static const struct vt_cell empty={.rune=' ',.fg=VT_DEFAULT,.bg=VT_DEFAULT,.width=1};
    if(row<0 || row>=v->rows || col<0 || col>=v->cols)return &empty;
    const struct vt_screen *s=v->alt?&v->alternate:&v->primary;
    if(v->alt)back=0;
    if(back>v->history_count)back=v->history_count;
    int logical=(int)v->history_count+row-(int)back;
    if(logical<(int)v->history_count) {
        unsigned slot=(v->history_head+VT_HISTORY-v->history_count+(unsigned)logical)%VT_HISTORY;
        return &v->history[slot].cells[col];
    }
    return &s->lines[s->order[logical-(int)v->history_count]].cells[col];
}
size_t vt_line_text(const struct vt *v,int row,unsigned back,char *dst,size_t cap) {
    size_t n=0;int last=-1;
    for(int x=0;x<v->cols;x++) {const struct vt_cell *c=vt_view_cell(v,row,x,back);if(c->rune!=' ' && c->width && !(c->flags&VT_HIDDEN))last=x;}
    for(int x=0;x<=last;x++) {
        const struct vt_cell *c=vt_view_cell(v,row,x,back);if(!c->width)continue;
        uint32_t runes[3]={c->rune,c->combining[0],c->combining[1]};
        if(c->flags&VT_HIDDEN){runes[0]=' ';runes[1]=runes[2]=0;}
        for(unsigned j=0;j<3;j++)if(runes[j]) {
            char bytes[4];unsigned k=vt_encode(runes[j],bytes);
            if(n+k>=cap)goto done;
            memcpy(dst+n,bytes,k);n+=k;
        }
    }
done:if(cap)dst[n]=0;return n;
}
static bool resize_primary(struct vt *v,int cols,int rows) {
    struct vt *tmp=calloc(1,sizeof *tmp);if(!tmp)return false;
    vt_init(tmp,cols,rows,NULL,NULL);
    int last=v->primary.cursor.y;
    for(int y=0;y<v->rows;y++)if(y>last && line_at(&v->primary,y)->used)last=y;
    uint64_t cursor_row=0;int cursor_col=0;bool cursor_wrap=false,found=false;
    for(unsigned i=0;i<v->history_count+(unsigned)last+1;i++) {
        bool hist=i<v->history_count;
        const struct vt_line *l=hist?&v->history[(v->history_head+VT_HISTORY-v->history_count+i)%VT_HISTORY]:line_at(&v->primary,(int)(i-v->history_count));
        unsigned used=l->used;
        bool current=!hist && (int)(i-v->history_count)==v->primary.cursor.y;
        if(l->wrapped)used=(unsigned)(v->cols-(l->wrap_pad?1:0));
        if(current && used<(unsigned)v->primary.cursor.x)used=(unsigned)v->primary.cursor.x;
        for(unsigned x=0;x<=used;x++) {
            if(current && x==(unsigned)v->primary.cursor.x && !v->primary.cursor.wrap) {
                cursor_row=tmp->scrolls+(unsigned)tmp->primary.cursor.y;cursor_col=tmp->primary.cursor.x;
                cursor_wrap=tmp->primary.cursor.wrap;found=true;
            }
            if(x<used && l->cells[x].width)put_cell(tmp,l->cells[x]);
        }
        if(current && v->primary.cursor.wrap) {
            cursor_row=tmp->scrolls+(unsigned)tmp->primary.cursor.y;cursor_col=tmp->primary.cursor.x;
            cursor_wrap=tmp->primary.cursor.wrap;found=true;
        }
        if(!l->wrapped && i<v->history_count+(unsigned)last) {tmp->primary.cursor.x=0;index_line(tmp,false);}
    }
    struct vt_cursor old=v->primary.cursor,saved=v->primary.saved;
    v->primary=tmp->primary;memcpy(v->history,tmp->history,sizeof v->history);
    v->history_head=tmp->history_head;v->history_count=tmp->history_count;
    if(found) {v->primary.cursor.x=cursor_col;v->primary.cursor.y=cursor_row<tmp->scrolls?0:clamp((int)(cursor_row-tmp->scrolls),0,rows-1);v->primary.cursor.wrap=cursor_wrap;}
    v->primary.cursor.fg=old.fg;v->primary.cursor.bg=old.bg;v->primary.cursor.flags=old.flags;
    v->primary.saved=v->alt_saved?v->primary.cursor:saved;
    v->primary.saved.x=clamp(v->primary.saved.x,0,cols-1);v->primary.saved.y=clamp(v->primary.saved.y,0,rows-1);
    v->scrolls+=tmp->scrolls;free(tmp);return true;
}
bool vt_resize(struct vt *v,int cols,int rows) {
    cols=clamp(cols,2,VT_COLS);rows=clamp(rows,2,VT_ROWS);
    if(cols==v->cols && rows==v->rows)return true;
    int oldrows=v->rows,oldcols=v->cols;
    if(!v->primary.addressed) {if(!resize_primary(v,cols,rows))return false;}
    else {
        /* Addressed screens preserve coordinates; the application can redraw
         * after querying its new size. Rows clipped here do not enter history. */
        struct vt_cursor c=v->primary.cursor;
        for(int y=oldrows;y<rows;y++)clear_line(line_at(&v->primary,y),&c);
    }
    v->cols=cols;v->rows=rows;
    struct vt_screen *screens[2]={&v->primary,&v->alternate};
    for(unsigned i=0;i<2;i++) {
        struct vt_screen *s=screens[i];
        for(int y=0;y<VT_ROWS;y++) {
            struct vt_line *l=line_at(s,y);
            if(y>=rows || (i && y>=oldrows))clear_line(l,&s->cursor);
            else {
                if(cols>oldcols && (i || s->addressed))for(int x=oldcols;x<cols;x++)l->cells[x]=blank(&s->cursor);
                normalize_to(l,cols,&s->cursor);
            }
        }
        s->cursor.x=clamp(s->cursor.x,0,cols-1);s->cursor.y=clamp(s->cursor.y,0,rows-1);
        s->saved.x=clamp(s->saved.x,0,cols-1);s->saved.y=clamp(s->saved.y,0,rows-1);
        if(s->cursor.x!=cols-1)s->cursor.wrap=false;
        s->top=0;s->bottom=rows-1;
    }
    v->revision++;return true;
}
size_t vt_key_sequence(const struct vt *v,unsigned key,unsigned mods,char out[16]) {
    unsigned modifier=1+(mods&7);size_t n=0;
    char final=0;
    switch(key) {
    case VT_KEY_UP:final='A';break;case VT_KEY_DOWN:final='B';break;
    case VT_KEY_RIGHT:final='C';break;case VT_KEY_LEFT:final='D';break;
    case VT_KEY_HOME:final='H';break;case VT_KEY_END:final='F';break;
    }
    if(key>=VT_KEY_F1 && key<VT_KEY_F1+4)final=(char)('P'+(key-VT_KEY_F1));
    if(final) {
        if(mods)return (size_t)snprintf(out,16,"\033[1;%u%c",modifier,final);
        out[0]=27;out[1]=key>=VT_KEY_F1 || v->application_cursor?'O':'[';out[2]=final;return 3;
    }
    static const unsigned char tilde[]={15,17,18,19,20,21,23,24};
    unsigned code=key==VT_KEY_DELETE?3:key==VT_KEY_INSERT?2:key==VT_KEY_PAGEUP?5:key==VT_KEY_PAGEDOWN?6:
                  key==VT_KEY_MENU?29:key>=VT_KEY_F1+4 && key<=VT_KEY_F12?tilde[key-VT_KEY_F1-4]:0;
    if(code)return (size_t)snprintf(out,16,mods?"\033[%u;%u~":"\033[%u~",code,modifier);
    if(key>=VT_KEY_KP_0 && key<=VT_KEY_KP_SLASH) {
        static const char app[]="pqrstuvwxynMkmjo",num[]="0123456789.\r+-*/";
        unsigned i=key-VT_KEY_KP_0;
        if(v->keypad_application){out[0]=27;out[1]='O';out[2]=app[i];return 3;}
        key=(unsigned char)num[i];
    }
    if(key==VT_KEY_TAB) {if(mods&1){memcpy(out,"\033[Z",3);return 3;}key='\t';}
    else if(key==VT_KEY_BACKSPACE)key=((mods&4)!=0)!=v->backspace_sends_bs?8:127;
    else if(key==VT_KEY_ENTER)key='\r';
    else if(key==VT_KEY_ESCAPE)key=27;
    if(key<256) {if((mods&2) && v->alt_sends_escape)out[n++]=27;out[n++]=(char)key;return n;}
    return 0;
}
