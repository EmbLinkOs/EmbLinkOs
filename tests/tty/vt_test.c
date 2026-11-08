/* vt_test.c -- the screen model (user/terminal/vt*.c), host-run. Every
 * sequence the model answers is driven here and checked against what xterm
 * does; the structural invariants are checked after random input, random
 * resizes and resets. `vt_test --fuzz N` runs N fuzz rounds (the ASan/UBSan
 * build runs many). */
#include "vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct vt v,other;
static bool live;
static char replies[8192];static size_t reply_length;
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort();}}while(0)
static void reply_cb(void *unused,const char *bytes,size_t n) {
    (void)unused;CHECK(reply_length+n<sizeof replies);memcpy(replies+reply_length,bytes,n);reply_length+=n;replies[reply_length]=0;
}
static void start(int cols,int rows) {
    if (live) vt_free(&v);
    CHECK(vt_init(&v,cols,rows,reply_cb,NULL));live=true;reply_length=0;replies[0]=0;
}
static void clear_replies(void) {reply_length=0;replies[0]=0;}
static void feed(const char *s) {vt_feed(&v,s,strlen(s));}
static const struct vt_cell *cell(int y,int x) {return vt_view_cell(&v,y,x,0);}
static uint32_t cp(int y,int x) {return cell(y,x)->cp;}
static const struct vt_style *style(int y,int x) {return vt_style_of(&v,cell(y,x));}
static void text_at(int row,unsigned back,const char *expected) {
    char s[VT_MAX_COLS*12+1];vt_line_text(&v,row,back,s,sizeof s);
    if (strcmp(s,expected)) fprintf(stderr,"row %d back %u: \"%s\", want \"%s\"\n",row,back,s,expected);
    CHECK(!strcmp(s,expected));
}
static void text(int row,const char *expected) {text_at(row,0,expected);}
static struct vt_cursor *cur(void) {return &vt_active(&v)->cursor;}

static void invariant(const struct vt *t) {
    CHECK(t->cols>=2 && t->cols<=VT_MAX_COLS && t->rows>=2 && t->rows<=VT_MAX_ROWS && t->row_cap>=t->cols);
    CHECK(t->nparam<=32 && t->string_length<sizeof t->string && t->utf8_left<=3);
    CHECK(t->nstyles>=1 && t->nstyles<=65535 && t->nlinks<=65536);
    CHECK(!memcmp(&t->styles[0],&(struct vt_style){0},sizeof t->styles[0]));
    const struct vt_screen *screens[2]={&t->primary,&t->alternate};
    for (int n=0;n<2;n++) {
        const struct vt_screen *s=screens[n];
        CHECK(s->cursor.x>=0 && s->cursor.x<t->cols && s->cursor.y>=0 && s->cursor.y<t->rows);
        CHECK(s->top>=0 && s->top<s->bottom && s->bottom<t->rows);
        for (int y=0;y<t->rows;y++) {
            for (int z=0;z<y;z++) CHECK(s->rows[y]!=s->rows[z]);
            const struct vt_cell *c=s->rows[y]->cells;
            for (int x=0;x<t->cols;x++) {
                CHECK(c[x].width<=2 && c[x].style<t->nstyles);
                if (c[x].flags&VT_CELL_CLUSTER) CHECK(c[x].cp<t->nclusters);
                if (c[x].width==2) CHECK(x+1<t->cols && c[x+1].width==0);
                if (!c[x].width) CHECK(x>0 && c[x-1].width==2);
            }
        }
    }
    uint32_t rows=0;size_t bytes=0;
    CHECK(t->history_lines<=t->history_cap);
    for (uint32_t i=0;i<t->history_lines;i++) {
        const struct vt_hline *l=&t->history[(t->history_head+i)%t->history_cap];
        rows+=l->rows;bytes+=l->n*sizeof *l->cells+sizeof *l;
        CHECK(l->rows>=1);
        for (uint32_t x=0;x<l->n;x++) {
            CHECK(l->cells[x].style<t->nstyles);
            if (l->cells[x].flags&VT_CELL_CLUSTER) CHECK(l->cells[x].cp<t->nclusters);
        }
    }
    CHECK(rows==t->history_rows && bytes==t->history_bytes);
    CHECK(!t->history_lines || t->history_bytes<=t->history_limit);
}
/* Two models in the same state, compared by what they show and hold. */
static bool same_cell(const struct vt *a,const struct vt_cell *x,const struct vt *b,const struct vt_cell *y) {
    char ta[64],tb[64];vt_cell_text(a,x,ta);vt_cell_text(b,y,tb);
    return x->width==y->width && !strcmp(ta,tb) && !memcmp(vt_style_of(a,x),vt_style_of(b,y),sizeof(struct vt_style));
}
static void same(const struct vt *a,const struct vt *b) {
    CHECK(a->cols==b->cols && a->rows==b->rows && a->alt==b->alt && a->history_rows==b->history_rows);
    const struct vt_screen *sa[2]={&a->primary,&a->alternate},*sb[2]={&b->primary,&b->alternate};
    for (int n=0;n<2;n++) {
        CHECK(sa[n]->cursor.x==sb[n]->cursor.x && sa[n]->cursor.y==sb[n]->cursor.y && sa[n]->cursor.wrap==sb[n]->cursor.wrap);
        CHECK(!memcmp(&sa[n]->cursor.pen,&sb[n]->cursor.pen,sizeof sa[n]->cursor.pen));
        for (int y=0;y<a->rows;y++) {
            CHECK(sa[n]->rows[y]->wrapped==sb[n]->rows[y]->wrapped);
            for (int x=0;x<a->cols;x++) CHECK(same_cell(a,&sa[n]->rows[y]->cells[x],b,&sb[n]->rows[y]->cells[x]));
        }
    }
    for (unsigned back=1;back<=a->history_rows && back<=64;back++)
        for (int x=0;x<a->cols;x++) CHECK(same_cell(a,vt_view_cell(a,0,x,back),b,vt_view_cell(b,0,x,back)));
    CHECK(!strcmp(a->title,b->title) && a->mouse==b->mouse && a->bracketed_paste==b->bracketed_paste);
}

static void basic(void) {
    start(10,4);feed("abcdef\rXY");text(0,"XYcdef");feed("\bZ");text(0,"XZcdef");
    feed("\033[3;4H!");CHECK(v.primary.cursor.x==4 && v.primary.cursor.y==2);CHECK(cp(2,3)=='!');
    feed("\033[999999999999A\033[999999999999D");CHECK(v.primary.cursor.x==0 && v.primary.cursor.y==0);
    feed("\033[999999999999B\033[999999999999C");CHECK(v.primary.cursor.x==9 && v.primary.cursor.y==3);
    feed("\033[2J");CHECK(v.primary.cursor.x==9 && v.primary.cursor.y==3);text(0,"");
    feed("\033[HabcdefghijK");text(0,"abcdefghij");text(1,"K");CHECK(vt_view_wrapped(&v,0,0) && !vt_view_wrapped(&v,1,0));
    feed("\033[?7l\033[1;10HXY");text(0,"abcdefghiY");CHECK(v.primary.cursor.x==9);
    feed("\033[?7h\033[H\033[2Jabcdef\033[1;3H\033[2@");text(0,"ab  cdef");
    feed("\033[2P");text(0,"abcdef");feed("\033[2X");text(0,"ab  ef");
    feed("\033[1K");text(0,"    ef");feed("\033[2K");text(0,"");
    feed("\033[H\033[32mA\0337\033[4;9H\033[31mB\0338C");text(0,"AC");CHECK(style(0,1)->fg==VT_COLOR_INDEX(2));
    feed("\033[6n\033[5n\033[c");CHECK(!strcmp(replies,"\033[1;3R\033[0n\033[?62;22c"));
    start(20,3);feed("a\tb");CHECK(cp(0,8)=='b');feed("\033[3g\r\t");CHECK(v.primary.cursor.x==19);
    feed("\033[1;5H\033H\r\t");CHECK(v.primary.cursor.x==4);feed("\033[Z");CHECK(v.primary.cursor.x==0);
    feed("\033[2I");CHECK(v.primary.cursor.x==19);   /* only the one stop left: CHT runs to the edge */
    /* CUU/CUD stop at the margins when the cursor is inside them */
    start(10,8);feed("\033[3;6r\033[5;1H\033[9A");CHECK(cur()->y==2);feed("\033[9B");CHECK(cur()->y==5);
    feed("\033[8;1H\033[9A");CHECK(cur()->y==2);feed("\033[1;1H\033[B");CHECK(cur()->y==1);
    feed("\033[r\033[1;1H\033[9E");CHECK(cur()->y==7 && cur()->x==0);
    /* IL/DL put the cursor in the first column */
    feed("\033[2;5H\033[L");CHECK(cur()->x==0 && cur()->y==1);
    /* REP repeats the last character printed, and nothing after a control */
    start(20,3);feed("a\033[3b");text(0,"aaaa");feed("\r\n\033[3b");text(1,"");
    feed("\xe7\x95\x8c\033[2b");text(1,"\xe7\x95\x8c\xe7\x95\x8c\xe7\x95\x8c");
    /* insert mode and LNM */
    start(20,3);feed("abc\r\033[4hXY\033[4l");text(0,"XYabc");feed("Z");text(0,"XYZbc");
    feed("\033[20h\nq");CHECK(cur()->y==1);text(1,"q");feed("\033[20l\nr");text(2," r");
    /* DECALN: the ESC # 8 test pattern, margins reset, cursor home */
    start(5,3);feed("\033[2;3r\033#8");text(0,"EEEEE");text(2,"EEEEE");CHECK(cur()->x==0 && cur()->y==0 && v.primary.bottom==2);
    feed("\033(B\033)0\016q\017q");text(0,"\xe2\x94\x80qEEE");          /* SO to G1 = graphics, SI back */
    /* BEL counts; DEL is ignored */
    feed("\a\a\177");CHECK(v.bells==2);
}
static void scrolling(void) {
    start(6,5);feed("a\r\nb\r\nc\r\nd\r\ne");
    feed("\033[2;4r\033[4;1H\n");text(0,"a");text(1,"c");text(2,"d");text(3,"");text(4,"e");CHECK(v.history_lines==0);
    feed("\033[2;1H\033M");text(1,"");text(2,"c");text(3,"d");
    feed("\033[?6h\033[1;1HX\033[999;1HY");CHECK(cp(1,0)=='X' && cp(3,0)=='Y');
    feed("\033[?6n");CHECK(!strcmp(replies,"\033[?3;2R"));feed("\033[?6l\033[r");
    feed("\033[2;1H\033[L");text(1,"");text(2,"X");feed("\033[M");text(1,"X");
    start(4,2);feed("1\r\n2\r\n3");CHECK(v.history_lines==1);text(0,"2");text(1,"3");
    text_at(0,1,"1");text_at(1,1,"2");
    for (int i=0;i<350;i++) feed("\r\nx");
    CHECK(v.history_lines==351 && v.history_rows==351);
    feed("\033[3J");CHECK(v.history_lines==0 && v.history_rows==0);text(1,"x");
    /* the cap is bytes: the oldest lines go */
    start(10,3);vt_set_history_limit(&v,10*(10*sizeof(struct vt_cell)+sizeof(struct vt_hline)));
    for (int i=0;i<40;i++) {char line[32];snprintf(line,sizeof line,"\r\nline%02d__x",i);feed(line);}
    invariant(&v);CHECK(v.history_lines==10);text_at(0,10,"line27__x");
    vt_set_history_limit(&v,0);CHECK(v.history_lines==0);invariant(&v);
    /* a wrapped line enters the scrollback whole and is read back at the width */
    start(4,2);feed("abcdefghij\r\nz\r\n\r\n");
    CHECK(v.history_lines==2 && v.history_rows==4);
    text_at(0,4,"abcd");text_at(0,3,"efgh");text_at(0,2,"ij");text_at(0,1,"z");
    CHECK(vt_view_wrapped(&v,0,4) && vt_view_wrapped(&v,0,3) && !vt_view_wrapped(&v,0,2));
    /* SU into the scrollback, SD not; scrolled-in rows take the pen's background */
    start(4,3);feed("a\r\nb\r\nc\033[44m\033[S");CHECK(v.history_lines==1);text(0,"b");text(2,"");
    CHECK(style(2,0)->bg==VT_COLOR_INDEX(4) && !style(2,0)->attrs);
    feed("\033[T");CHECK(v.history_lines==1);text(1,"b");
}
static void alternate_test(void) {
    start(12,4);feed("primary\033[32m\033[?1049h\033[2J\033[Halt");CHECK(v.alt);text(0,"alt");
    feed("\033[?1049hX");text(0,"altX");
    for (int i=0;i<20;i++) feed("\r\nsecondary");
    CHECK(v.history_lines==0);
    feed("\033[?25l\033[?1h\033[?2004h\033[5 q");CHECK(!v.cursor_visible && v.application_cursor && v.bracketed_paste && v.cursor_style==5);
    feed("\033[?1049lZ");CHECK(!v.alt);text(0,"primaryZ");CHECK(style(0,7)->fg==VT_COLOR_INDEX(2));
    feed("\033[?1049l");text(0,"primaryZ");feed("\033[?1049h");text(0,"");feed("\033[?1049l");text(0,"primaryZ");
    /* the cursor stays where it is across a switch; 47 keeps the alternate
     * screen, 1047 clears it on the way out, 1049 on the way in */
    feed("\033[?47hkeep");CHECK(cur()->x==12-1 && cur()->y==0);
    feed("\033[H\033[2Kkeep\033[?47l\033[?47h");text(0,"keep");feed("\033[?1047h");text(0,"keep");
    feed("\033[?1047l\033[?1047h");text(0,"");feed("\033[?1047l");
    /* the scrollback is the primary screen's: none of it shows in the alternate */
    feed("\r\n\r\n\r\n\r\nmore");CHECK(v.history_lines>0);feed("\033[?1049h");text_at(0,5,"");
}
static void colours(void) {
    start(20,3);feed("\033[1;2;3;4;7;8;9;38;5;196;48;2;12;34;56mX");
    const struct vt_style *s=style(0,0);
    CHECK(s->fg==VT_COLOR_INDEX(196) && s->bg==VT_COLOR_RGB(0x0c2238) && s->underline==VT_UL_SINGLE);
    CHECK(s->attrs==(VT_BOLD|VT_DIM|VT_ITALIC|VT_INVERSE|VT_HIDDEN|VT_STRIKE));
    CHECK(vt_rgb(&v,s->fg,true)==0xff0000 && vt_rgb(&v,s->bg,false)==0x0c2238);
    feed("\033[22;23;24;27;28;29;39;49mY");s=style(0,1);CHECK(!s->attrs && !s->underline && s->fg==VT_COLOR_DEFAULT && s->bg==VT_COLOR_DEFAULT);
    CHECK(cell(0,1)->style==0);
    feed("\033[94;103mZ");CHECK(style(0,2)->fg==VT_COLOR_INDEX(12) && style(0,2)->bg==VT_COLOR_INDEX(11));
    feed("\033[0;38;2;2mW");CHECK(style(0,3)->fg==VT_COLOR_DEFAULT && !style(0,3)->attrs);
    /* BCE: an erase takes the pen's background and nothing else */
    feed("\033[1;4;31;48;5;21m\033[K");s=style(0,9);CHECK(s->bg==VT_COLOR_INDEX(21) && s->fg==VT_COLOR_DEFAULT && !s->attrs && !s->underline);
    /* the colon forms, underline styles and colours, blink, overline */
    start(20,3);
    feed("\033[0;38:2::1:2:3;48:2:4:5:6;58:5:100;4:3;5;53mA");s=style(0,0);
    CHECK(s->fg==VT_COLOR_RGB(0x010203) && s->bg==VT_COLOR_RGB(0x040506) && s->ul==VT_COLOR_INDEX(100));
    CHECK(s->underline==VT_UL_CURLY && s->attrs==(VT_BLINK|VT_OVERLINE));
    feed("\033[21;6;58;2;9;8;7mB");s=style(0,1);CHECK(s->underline==VT_UL_DOUBLE && (s->attrs&VT_RAPID) && s->ul==VT_COLOR_RGB(0x090807));
    feed("\033[4:0;25;55;59;38:5:7mC");s=style(0,2);CHECK(!s->underline && !(s->attrs&(VT_BLINK|VT_RAPID|VT_OVERLINE)) && s->ul==VT_COLOR_DEFAULT && s->fg==VT_COLOR_INDEX(7));
    feed("\033[4:4mD\033[4:5mE\033[24mF");CHECK(style(0,3)->underline==VT_UL_DOTTED && style(0,4)->underline==VT_UL_DASHED && !style(0,5)->underline);
    feed("\033[m\033[38;5;300mG");CHECK(style(0,6)->fg==VT_COLOR_DEFAULT);      /* out of range: unchanged */
    feed("\033[38;7;1mH");CHECK(cell(0,7)->style==0);                           /* unknown kind: the rest is skipped */
    feed("\033[10;20;51;60mI");CHECK(cell(0,8)->style==0);                      /* fonts, frames, ideograms: no look */
    /* a palette change recolours what is already on the screen */
    start(10,2);feed("\033[31mR\033[mD");
    struct vt_palette pal=v.base;pal.color[1]=0x123456;pal.fg=0xabcdef;vt_set_palette(&v,&pal);
    CHECK(vt_rgb(&v,style(0,0)->fg,true)==0x123456 && vt_rgb(&v,style(0,1)->fg,true)==0xabcdef);
    CHECK(vt_default_color(196)==0xff0000 && vt_default_color(232)==0x080808 && vt_default_color(16)==0);
}
static void unicode_test(void) {
    start(6,3);feed("caf\xc3");feed("\xa9 e\xcc");feed("\x81");text(0,"caf\xc3\xa9 e\xcc\x81");
    CHECK(cell(0,5)->flags&VT_CELL_CLUSTER);
    char t[64];vt_cell_text(&v,cell(0,5),t);CHECK(!strcmp(t,"e\xcc\x81"));
    feed("\xe7\x95\x8c");CHECK(cp(1,0)==0x754c && cell(1,0)->width==2 && !cell(1,1)->width);
    feed("\033[2;2HX");CHECK(cp(1,0)==' ' && cell(1,0)->width==1 && cp(1,1)=='X');
    feed("\033[2;1H\xe7\x95\x8c\033[2;1H\033[P");invariant(&v);
    start(4,3);feed("abc\xe7\x95\x8c");text(0,"abc");CHECK(cell(1,0)->width==2 && v.primary.rows[0]->pad);
    feed("\033[?7l\033[1;4H\xe7\x95\x8c");CHECK(cp(0,3)==0xfffd && cell(0,3)->width==1);
    start(12,3);feed("\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80");CHECK(cp(0,0)==0xfffd && cp(0,1)==0xfffd && cp(0,2)==0xfffd && cp(0,3)==0xfffd);
    feed("\033(0lqqk\033(B");CHECK(cp(0,4)==0x250c && cp(0,5)==0x2500 && cp(0,7)==0x2510);
    /* a mark after a wrap joins the last cell of the row before */
    start(3,3);feed("abc\xcc\x81");vt_cell_text(&v,cell(0,2),t);CHECK(!strcmp(t,"c\xcc\x81"));
    feed("d\r\n\xcc\x81");text(1,"d");                       /* after CR LF there is nothing to join */
    /* seven marks a cell; more are dropped */
    start(5,2);feed("o");for (int i=0;i<10;i++) feed("\xcc\x88");
    vt_cell_text(&v,cell(0,0),t);CHECK(strlen(t)==1+7*2);
    /* a mark on the right half of a wide character joins the character */
    start(5,2);feed("\xe7\x95\x8c\xcc\x81");vt_cell_text(&v,cell(0,0),t);CHECK(!strcmp(t,"\xe7\x95\x8c\xcc\x81"));
}
static void resize_test(void) {
    start(4,3);feed("abc\xe7\x95\x8c");CHECK(vt_resize(&v,8,3));text(0,"abc\xe7\x95\x8c");
    CHECK(v.primary.cursor.x==5 && v.primary.cursor.y==0);
    start(5,4);feed("abcdefghij");CHECK(v.primary.cursor.wrap);CHECK(vt_resize(&v,10,4));text(0,"abcdefghij");
    CHECK(v.primary.cursor.wrap);feed("K");text(1,"K");
    CHECK(vt_resize(&v,4,4));text(0,"abcd");text(1,"efgh");text(2,"ijK");
    feed("\033[?1049h\033[Halternate");CHECK(vt_resize(&v,8,5));CHECK(v.alt);feed("\033[?1049l");text(0,"abcdefgh");text(1,"ijK");
    start(6,5);feed("a\r\nb\r\nc\r\nd\r\ne");CHECK(vt_resize(&v,6,3));text(0,"c");text(2,"e");CHECK(v.history_lines==2);
    CHECK(vt_resize(&v,6,6));text(0,"a");text(4,"e");CHECK(v.history_lines==0 && cur()->y==4);
    start(8,4);feed("\033[3;4H\xe7\x95\x8c");CHECK(vt_resize(&v,4,3));invariant(&v);
    /* reflow keeps working after the screen is cleared and the cursor addressed */
    start(10,4);feed("\033[H\033[2Jhello world, wrapped\033[2;3H");CHECK(vt_resize(&v,5,6));
    text(0,"hello");text(1," worl");text(2,"d, wr");text(3,"apped");
    CHECK(cur()->y==2 && cur()->x==2);       /* the cursor stays on its character: the space after "d," */
    CHECK(vt_resize(&v,40,4));text(0,"hello world, wrapped");CHECK(cur()->y==0 && cur()->x==12);
    /* the scrollback re-wraps at the new width */
    start(6,2);feed("abcdefghijkl\r\nxy\r\nzz");CHECK(v.history_lines==1 && v.history_rows==2);
    CHECK(vt_resize(&v,4,2));text_at(0,3,"abcd");text_at(0,2,"efgh");text_at(0,1,"ijkl");text(0,"xy");text(1,"zz");
    CHECK(vt_resize(&v,12,2));text_at(0,1,"abcdefghijkl");invariant(&v);
    /* an open scrollback line comes back to join the rest of it on the screen */
    start(4,2);feed("abcdefghij");CHECK(v.history_lines==1 && v.history[0].open);
    CHECK(vt_resize(&v,10,2));CHECK(v.history_lines==0);text(0,"abcdefghij");CHECK(cur()->wrap && cur()->x==9);
    /* a full screen growing taller brings the scrollback back (full: written
     * to its last row, wherever the cursor is); one that was not full just
     * gains blank rows */
    start(5,3);feed("1\r\n2\r\n3\r\n4\r\n5");CHECK(vt_resize(&v,5,5));text(0,"1");text(4,"5");CHECK(cur()->y==4);
    start(5,3);feed("1\r\n2\r\n3\r\n4\r\n5\033[H");CHECK(vt_resize(&v,5,5));text(0,"1");text(2,"3");CHECK(cur()->y==2);
    start(5,3);feed("1\r\n2\r\n3\r\n4\r\n5\033[2J\033[Hx");CHECK(vt_resize(&v,5,5));text(0,"x");CHECK(v.history_lines==2 && cur()->y==0);
    /* rows below the cursor that do not fit are cut; the cursor's row stays */
    start(5,4);feed("a\r\nb\r\nc\r\nd\033[1;1H");CHECK(vt_resize(&v,5,2));text(0,"a");text(1,"b");CHECK(cur()->y==0);
    /* the alternate screen is clipped, not reflowed */
    start(6,3);feed("\033[?1049habcdefgh");CHECK(vt_resize(&v,3,3));text(0,"abc");text(1,"gh");
    CHECK(vt_resize(&v,6,3));text(0,"abc");text(1,"gh");
    /* the cursor saved by 1049 moves with its text */
    start(8,4);feed("one two three four\033[?1049h");CHECK(vt_resize(&v,4,6));feed("\033[?1049l");
    text(0,"one");text(4,"ur");CHECK(cur()->y==4 && cur()->x==2);
    /* big and small */
    start(VT_MAX_COLS,VT_MAX_ROWS);feed("\033[500;1000HX");CHECK(cp(VT_MAX_ROWS-1,VT_MAX_COLS-1)=='X');
    CHECK(vt_resize(&v,2,2));CHECK(vt_resize(&v,VT_MAX_COLS,VT_MAX_ROWS));invariant(&v);
    /* the prompt's row moves with the reflow */
    start(10,5);feed("0123456789abc\r\n\033]133;A;edit=shell;cwd=/\007$ ");CHECK(v.prompt_row-(int64_t)v.scrolls==2);
    CHECK(vt_resize(&v,5,5));CHECK(v.prompt_row-(int64_t)v.scrolls==3);text(3,"$");
}
static void strings_and_keys(void) {
    start(20,4);feed("\033]2;Device log\007X\033Pignored text\033\\Y\033]8;;https://invalid.example\033\\Z");text(0,"XYZ");CHECK(!strcmp(v.title,"Device log"));
    CHECK(vt_link_uri(&v,style(0,2)->link) && !strcmp(vt_link_uri(&v,style(0,2)->link),"https://invalid.example"));
    feed("\033[999999999999999;99999999999999H!");CHECK(cp(3,19)=='!');
    feed("\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31;32;33;34H");invariant(&v);
    char b[16];size_t n=vt_key_sequence(&v,VT_KEY_UP,0,b);CHECK(n==3 && !memcmp(b,"\033[A",3));
    feed("\033[?1h");n=vt_key_sequence(&v,VT_KEY_LEFT,0,b);CHECK(n==3 && !memcmp(b,"\033OD",3));
    n=vt_key_sequence(&v,VT_KEY_UP,4,b);CHECK(n==6 && !memcmp(b,"\033[1;5A",6));
    n=vt_key_sequence(&v,19,4,b);CHECK(n==1 && b[0]==19);
    n=vt_key_sequence(&v,VT_KEY_TAB,1,b);CHECK(n==3 && !memcmp(b,"\033[Z",3));
    n=vt_key_sequence(&v,'x',2,b);CHECK(n==2 && !memcmp(b,"\033x",2));
    /* xterm's PC-style keys, each against the bytes xterm sends. */
    static const struct {unsigned key,mods;const char *want;} keys[]={
        {VT_KEY_F1,0,"\033OP"},{VT_KEY_F1+3,0,"\033OS"},{VT_KEY_F1+2,1,"\033[1;2R"},{VT_KEY_F1+4,0,"\033[15~"},
        {VT_KEY_F1+5,0,"\033[17~"},{VT_KEY_F1+9,0,"\033[21~"},{VT_KEY_F1+10,0,"\033[23~"},{VT_KEY_F12,5,"\033[24;6~"},
        {VT_KEY_LEFT,4,"\033[1;5D"},{VT_KEY_RIGHT,6,"\033[1;7C"},{VT_KEY_HOME,1,"\033[1;2H"},{VT_KEY_END,0,"\033OF"},
        {VT_KEY_INSERT,0,"\033[2~"},{VT_KEY_DELETE,4,"\033[3;5~"},{VT_KEY_PAGEUP,1,"\033[5;2~"},{VT_KEY_MENU,0,"\033[29~"},
        {VT_KEY_BACKSPACE,0,"\177"},{VT_KEY_BACKSPACE,4,"\b"},{VT_KEY_BACKSPACE,2,"\033\177"},
        {VT_KEY_ENTER,0,"\r"},{VT_KEY_ENTER,2,"\033\r"},{VT_KEY_ESCAPE,0,"\033"},{VT_KEY_TAB,0,"\t"},
        {VT_KEY_KP_0+5,0,"5"},{VT_KEY_KP_ENTER,0,"\r"},{VT_KEY_KP_STAR,0,"*"},
    };
    for (unsigned i=0;i<sizeof keys/sizeof keys[0];i++) {
        n=vt_key_sequence(&v,keys[i].key,keys[i].mods,b);
        CHECK(n==strlen(keys[i].want) && !memcmp(b,keys[i].want,n));
    }
    feed("\033=");CHECK(v.keypad_application);
    n=vt_key_sequence(&v,VT_KEY_KP_0+5,0,b);CHECK(n==3 && !memcmp(b,"\033Ou",3));
    n=vt_key_sequence(&v,VT_KEY_KP_ENTER,0,b);CHECK(n==3 && !memcmp(b,"\033OM",3));
    n=vt_key_sequence(&v,VT_KEY_KP_SLASH,0,b);CHECK(n==3 && !memcmp(b,"\033Oo",3));
    feed("\033>");CHECK(!v.keypad_application);
    feed("\033[?66h");CHECK(v.keypad_application);feed("\033[?66l");CHECK(!v.keypad_application);
    feed("\033[20h");n=vt_key_sequence(&v,VT_KEY_ENTER,0,b);CHECK(n==2 && !memcmp(b,"\r\n",2));feed("\033[20l");
    v.backspace_sends_bs=true;n=vt_key_sequence(&v,VT_KEY_BACKSPACE,0,b);CHECK(n==1 && b[0]==8);
    n=vt_key_sequence(&v,VT_KEY_BACKSPACE,4,b);CHECK(n==1 && b[0]==127);
    feed("\033c");CHECK(v.backspace_sends_bs && v.alt_sends_escape);   /* settings survive a reset */
    feed("\033[?67l");CHECK(!v.backspace_sends_bs);
    v.alt_sends_escape=false;n=vt_key_sequence(&v,'x',2,b);CHECK(n==1 && b[0]=='x');
    feed("\033[?1036h");CHECK(v.alt_sends_escape);
    feed("\033]133;A;cwd=/home/tester\007");CHECK(v.shell_ready && !strcmp(v.directory,"/home/tester"));
    feed("\033]133;C\007");CHECK(!v.shell_ready && v.prompt_serial==2);
    feed("\033c");CHECK(!v.alt && !v.application_cursor && v.cursor_visible && !v.bracketed_paste && !v.history_lines);text(0,"");
    /* Shell integration: A's fields (cwd runs to the end, so it may hold
     * ';'), D's status, the in-band size report, and the prompt erase. */
    start(20,6);feed("x\r\n\033]133;A;edit=shell;cwd=/a;b\007$ typed");
    CHECK(v.shell_ready && v.edit_shell && !strcmp(v.directory,"/a;b") && v.prompt_row==1);
    feed("\033]133;A;cwd=/plain\007");CHECK(!v.edit_shell && !strcmp(v.directory,"/plain"));
    feed("\033]133;A;edit=shell;cwd=/\007\033]133;C;cmdline=ls\007\033]133;D;3\007");CHECK(!v.shell_ready && v.last_status==3);
    clear_replies();feed("\033[?2048h");CHECK(v.inband_resize && !strcmp(replies,"\033[48;6;20;0;0t"));
    start(20,6);feed("out\r\n\033]133;A;edit=shell;cwd=/\007$ long line");vt_prompt_erase(&v);
    text(1,"$ long line");                       /* reports off: the shell would not redraw */
    feed("\033[?2048h");vt_prompt_erase(&v);text(0,"out");text(1,"");CHECK(v.primary.cursor.x==0 && v.primary.cursor.y==1);
    feed("$ again\033]133;R\007");CHECK(cur()->x==0 && cur()->y==1);
    vt_prompt_here(&v);CHECK(v.prompt_row==(int64_t)v.scrolls+1);
}
static void replies_test(void) {
    start(20,5);v.pixel_width=200;v.pixel_height=100;
    /* DA2, XTVERSION, DA3 */
    clear_replies();feed("\033[>c\033[>q\033[=c");CHECK(!strcmp(replies,"\033[>1;10;0c\033P>|EmbLinkTerminal(1.0)\033\\\033P!|00000000\033\\"));
    /* DECRQM: private and ANSI */
    clear_replies();feed("\033[?2004$p\033[?2004h\033[?2004$p\033[?9999$p\033[?8$p");
    CHECK(!strcmp(replies,"\033[?2004;2$y\033[?2004;1$y\033[?9999;0$y\033[?8;3$y"));
    clear_replies();feed("\033[4$p\033[4h\033[4$p\033[4l\033[99$p");CHECK(!strcmp(replies,"\033[4;2$y\033[4;1$y\033[99;0$y"));
    /* window reports */
    clear_replies();feed("\033[14t\033[16t\033[18t\033[19t\033[11t");
    CHECK(!strcmp(replies,"\033[4;100;200t\033[6;20;10t\033[8;5;20t\033[9;5;20t\033[1t"));
    clear_replies();feed("\033[21t\033[8;10;10t");CHECK(!reply_length && v.cols==20);   /* no title report, no resizing */
    /* DECRQSS */
    clear_replies();feed("\033[1;3;4:3;38;5;100;48;2;1;2;3;58:5:9m\033P$qm\033\\");
    CHECK(!strcmp(replies,"\033P1$r0;1;3;4:3;38:5:100;48:2::1:2:3;58:5:9m\033\\"));
    clear_replies();feed("\033[m\033[31;102m\033P$qm\033\\\033[2;4r\033P$qr\033\\\033[4 q\033P$q q\033\\\033P$qx\033\\");
    CHECK(!strcmp(replies,"\033P1$r0;31;102m\033\\\033P1$r2;4r\033\\\033P1$r4 q\033\\\033P0$r\033\\"));
    /* XTGETTCAP: TN, RGB, an unknown one */
    clear_replies();feed("\033P+q544e;524742;7a7a\033\\");
    CHECK(!strcmp(replies,"\033P1+r544e=787465726D2D323536636F6C6F72\033\\\033P1+r524742=382F382F38\033\\\033P0+r7a7a\033\\"));
    /* colours: set, query (with the terminator the query used), reset */
    clear_replies();feed("\033]4;1;rgb:12/34/56;2;#abcdef\007\033]4;1;?\007\033]4;2;?\033\\");
    CHECK(!strcmp(replies,"\033]4;1;rgb:1212/3434/5656\007\033]4;2;rgb:abab/cdcd/efef\033\\"));
    CHECK(vt_rgb(&v,VT_COLOR_INDEX(1),true)==0x123456);
    feed("\033]104;1\007");CHECK(vt_rgb(&v,VT_COLOR_INDEX(1),true)==vt_default_color(1) && vt_rgb(&v,VT_COLOR_INDEX(2),true)==0xabcdef);
    feed("\033]104\007");CHECK(vt_rgb(&v,VT_COLOR_INDEX(2),true)==vt_default_color(2));
    feed("\033]4;3;rgb:f/80/fff\007");CHECK(vt_rgb(&v,VT_COLOR_INDEX(3),true)==0xff80ff);
    feed("\033]4;3;#fff000fff\007");CHECK(vt_rgb(&v,VT_COLOR_INDEX(3),true)==0xff00ff);
    feed("\033]4;3;nonsense\007");CHECK(vt_rgb(&v,VT_COLOR_INDEX(3),true)==0xff00ff);
    clear_replies();feed("\033]10;?\007\033]11;?\007");
    CHECK(!strcmp(replies,"\033]10;rgb:d9d9/dfdf/e7e7\007\033]11;rgb:1c1c/1f1f/2424\007"));
    feed("\033]10;#010203;#040506;#070809\007");
    CHECK(vt_rgb(&v,VT_COLOR_DEFAULT,true)==0x010203 && vt_rgb(&v,VT_COLOR_DEFAULT,false)==0x040506 && vt_cursor_rgb(&v)==0x070809);
    feed("\033]110\007\033]111\007\033]112\007");
    CHECK(vt_rgb(&v,VT_COLOR_DEFAULT,true)==0xd9dfe7 && vt_rgb(&v,VT_COLOR_DEFAULT,false)==0x1c1f24 && vt_cursor_rgb(&v)==0x6abec5);
    /* titles: OSC 0/1/2, the stack */
    feed("\033]0;both\007");CHECK(!strcmp(v.title,"both") && !strcmp(v.icon,"both"));
    feed("\033]1;icon\007");CHECK(!strcmp(v.title,"both") && !strcmp(v.icon,"icon"));
    feed("\033[22;0t\033]2;vim\007");CHECK(!strcmp(v.title,"vim"));feed("\033[23;0t");CHECK(!strcmp(v.title,"both"));
    feed("\033]2;caf\xc3\xa9\033\\");CHECK(!strcmp(v.title,"caf\xc3\xa9"));
    /* OSC 7 */
    feed("\033]7;file://host/home/me/a%20b\007");CHECK(!strcmp(v.directory,"/home/me/a b"));
    /* OSC 8: the id keeps two links to one place apart; an empty URI ends the link */
    start(20,2);feed("\033]8;id=1;http://a\033\\A\033]8;id=2;http://a\033\\B\033]8;id=1;http://a\033\\C\033]8;;\033\\D");
    CHECK(style(0,0)->link && style(0,0)->link==style(0,2)->link && style(0,1)->link!=style(0,0)->link && !style(0,3)->link);
    CHECK(!strcmp(vt_link_uri(&v,style(0,1)->link),"http://a"));
    feed("\033]8;;http://keep\033\\\033[0mE");CHECK(style(0,4)->link);       /* SGR 0 does not end a link */
    /* OSC 52: off unless allowed, and never read */
    start(20,2);feed("\033]52;c;aGVsbG8=\007");CHECK(!v.clipboard_pending);
    v.allow_clipboard_write=true;clear_replies();feed("\033]52;c;aGVsbG8gd29ybGQ=\007\033]52;c;?\007");
    CHECK(v.clipboard_pending && v.clipboard_len==11 && !strcmp(v.clipboard,"hello world") && !reply_length);
    /* XTSAVE / XTRESTORE */
    feed("\033[?2004h\033[?2004s\033[?2004l\033[?2004r");CHECK(v.bracketed_paste);
    /* DECSTR: modes back, the screen and the cursor's place kept */
    start(10,3);feed("text\033[?25l\033[4h\033[?6h\033[2;3r\033[1;31m\033[!p");
    CHECK(v.cursor_visible && !v.insert_mode && !cur()->origin && v.primary.top==0 && v.primary.bottom==2);
    feed("x");CHECK(!cell(0,4)->style);text(0,"text");
    /* modes that are flags for the renderer and the input side */
    feed("\033[?5h\033[?12h\033[?1004h\033[?1007h\033[?2026h");CHECK(v.reverse_video && v.cursor_blink && v.focus_events && v.alternate_scroll && v.sync_output);
    char f[4];CHECK(vt_focus_report(&v,true,f)==3 && !memcmp(f,"\033[I",3));CHECK(vt_focus_report(&v,false,f)==3 && !memcmp(f,"\033[O",3));
    feed("\033[?1004l\033[?2026l");CHECK(!vt_focus_report(&v,true,f) && !v.sync_output);
    /* unknown and malformed sequences change nothing */
    start(10,2);feed("\033[?1;2$xA\033[1;2;3 zB\033[<1uC\033[>4;2mD\033Ok");text(0,"ABCDk");   /* SS3: k from G3, ASCII */
    feed("\033[1\033[2mX");CHECK(style(0,5)->attrs==VT_DIM);   /* ESC inside CSI starts over */
    feed("\033]2;abc\033[mY");text(0,"ABCDkXY");CHECK(!strcmp(v.title,""));   /* ESC that is not ST abandons the string */
    feed("\033[12\030Z");text(0,"ABCDkXYZ");                                 /* CAN cancels */
}
static void mouse_test(void) {
    start(300,100);char b[48];size_t n;
    CHECK(!vt_mouse_report(&v,VT_MOUSE_PRESS,0,0,0,0,0,0,b));
    feed("\033[?1000h");
    n=vt_mouse_report(&v,VT_MOUSE_PRESS,0,4,2,0,0,0,b);CHECK(n==6 && !memcmp(b,"\033[M %#",6));
    n=vt_mouse_report(&v,VT_MOUSE_RELEASE,0,4,2,0,0,0,b);CHECK(n==6 && !memcmp(b,"\033[M#%#",6));
    n=vt_mouse_report(&v,VT_MOUSE_PRESS,2,0,0,0,0,5,b);CHECK(n==6 && b[3]==32+2+4+16);
    n=vt_mouse_report(&v,VT_MOUSE_PRESS,4,0,0,0,0,0,b);CHECK(n==6 && b[3]==32+64);
    CHECK(!vt_mouse_report(&v,VT_MOUSE_RELEASE,4,0,0,0,0,0,b));      /* the wheel has no release */
    CHECK(!vt_mouse_report(&v,VT_MOUSE_MOTION,0,1,1,0,0,0,b));      /* 1000: no motion */
    CHECK(!vt_mouse_report(&v,VT_MOUSE_PRESS,0,250,0,0,0,0,b));     /* past 223: X10 cannot say */
    feed("\033[?1002h");
    n=vt_mouse_report(&v,VT_MOUSE_MOTION,0,1,1,0,0,0,b);CHECK(n==6 && b[3]==32+32);
    CHECK(!vt_mouse_report(&v,VT_MOUSE_MOTION,3,1,1,0,0,0,b));      /* 1002: motion only with a button */
    feed("\033[?1003h");n=vt_mouse_report(&v,VT_MOUSE_MOTION,3,1,1,0,0,0,b);CHECK(n==6 && b[3]==32+35);
    feed("\033[?1006h");
    n=vt_mouse_report(&v,VT_MOUSE_PRESS,0,250,99,0,0,0,b);CHECK(n==strlen("\033[<0;251;100M") && !memcmp(b,"\033[<0;251;100M",n));
    n=vt_mouse_report(&v,VT_MOUSE_RELEASE,2,0,0,0,0,0,b);CHECK(n==strlen("\033[<2;1;1m") && !memcmp(b,"\033[<2;1;1m",n));
    feed("\033[?1006l\033[?1005h");
    n=vt_mouse_report(&v,VT_MOUSE_PRESS,0,250,0,0,0,0,b);CHECK(n==7 && (unsigned char)b[4]==0xc4 && (unsigned char)b[5]==0x9b);
    feed("\033[?1005l\033[?1015h");n=vt_mouse_report(&v,VT_MOUSE_PRESS,1,9,9,0,0,0,b);CHECK(n==strlen("\033[33;10;10M") && !memcmp(b,"\033[33;10;10M",n));
    feed("\033[?1015l\033[?1016h");n=vt_mouse_report(&v,VT_MOUSE_PRESS,0,9,9,95,97,0,b);CHECK(n==strlen("\033[<0;96;98M") && !memcmp(b,"\033[<0;96;98M",n));
    feed("\033[?1003l");CHECK(!vt_mouse_report(&v,VT_MOUSE_PRESS,0,1,1,0,0,0,b) && v.mouse==VT_MOUSE_OFF);
    feed("\033[?9h");n=vt_mouse_report(&v,VT_MOUSE_PRESS,0,0,0,0,0,7,b);CHECK(n>0 && !vt_mouse_report(&v,VT_MOUSE_RELEASE,0,0,0,0,0,0,b));
    feed("\033[?1016l");n=vt_mouse_report(&v,VT_MOUSE_PRESS,0,0,0,0,0,7,b);CHECK(n==6 && b[3]==32);   /* X10: no modifiers */
}
static void conceal(void) {
    start(20,3);feed("\033[8msecret\033[28m ok");text(0,"       ok");
}
/* More looks than the table holds: compaction keeps the ones in use, and a
 * scrollback full of them gives way. */
static void style_pressure(void) {
    start(40,10);
    char seq[64];
    for (int i=0;i<140000;i++) {
        snprintf(seq,sizeof seq,"\033[38;2;%d;%d;%dm#",(i>>16)&255,(i>>8)&255,i&255);feed(seq);
        if (i%40==39) feed("\r\n");
    }
    invariant(&v);CHECK(v.nstyles<=65535);
    /* the last row written keeps its own colours */
    const struct vt_style *s=vt_style_of(&v,vt_view_cell(&v,cur()->y-1,39,0));
    CHECK(s->fg==VT_COLOR_RGB(139999&0xffffff));
    /* many marks, many links */
    for (int i=0;i<5000;i++) {snprintf(seq,sizeof seq,"\033]8;;http://x/%d\033\\L\xcc\x81",i);feed(seq);}
    invariant(&v);
}

static uint32_t random_state=0x532985f;
static unsigned random_next(void) {random_state=random_state*1664525u+1013904223u;return random_state;}
/* Bytes that hit the parser's corners more often than uniform noise does. */
static size_t random_chunk(unsigned char *out,size_t cap) {
    static const char *const pieces[]={"\033[","\033]","\033P","\033\\","\a",";",":","?","\033[?1049h","\033[?1049l",
        "\033[2J","\r\n","\xe7\x95\x8c","\xcc\x81","\033[38:2::1:2:3m","\033]8;;u\033\\","\033#8","\033[3b","\033[4h",
        "\033[2;5r","\033M","\033[L","\033[P","\033[@","\033[S","\033[T","\033]133;A;edit=shell;cwd=/\007","\033]133;R\007"};
    size_t n=0;
    while (n<cap) {
        unsigned r=random_next()>>8;
        if (r%3==0) {
            const char *p=pieces[(r/3)%(sizeof pieces/sizeof *pieces)];size_t k=strlen(p);
            if (n+k>cap) break;
            memcpy(out+n,p,k);n+=k;
        } else out[n++]=(unsigned char)(r>>8);
    }
    return n;
}
static void fuzz(int rounds) {
    start(30,8);
    for (int round=0;round<rounds;round++) {
        unsigned char bytes[96];size_t n=random_chunk(bytes,sizeof bytes);
        vt_feed(&v,bytes,n);
        if (round%7==0) invariant(&v);
        if (round%29==0) {
            unsigned r=random_next();
            int cols=r&1?2+(int)(random_next()%60):2+(int)(random_next()%VT_MAX_COLS);
            int rows=r&2?2+(int)(random_next()%30):2+(int)(random_next()%VT_MAX_ROWS);
            CHECK(vt_resize(&v,cols,rows));invariant(&v);
            if (r&4) {vt_prompt_erase(&v);vt_prompt_here(&v);}
        }
        if (round%101==0) vt_reset(&v);
        if (round%211==0) {CHECK(vt_resize(&v,30,8));vt_set_history_limit(&v,(random_next()%64)*1024);}
        if (round%17==0) for (int y=0;y<v.rows;y++) {char s[VT_MAX_COLS*12+1];vt_line_text(&v,y,random_next()%200,s,sizeof s);}
    }
    invariant(&v);
}
static void split_and_fuzz(void) {
    const char *sample="hello\r\n\033[?1049h\033[H\033[38;2;1;2;3m\xe7\x95\x8c\xcc\x81\033[2;2H!\033]2;Title\033\\\033[?1049lworld"
                       "\033[4:3;58:5:9mx\033]8;id=a;http://l\033\\link\033]8;;\033\\\r\n\033P+q544e\033\\\033[3bwrapping past the edge";
    start(30,8);vt_feed(&v,sample,strlen(sample));
    CHECK(vt_init(&other,30,8,reply_cb,NULL));
    for (size_t i=0;i<strlen(sample);i++) vt_feed(&other,sample+i,1);
    same(&v,&other);
    vt_free(&other);
    fuzz(1500);
}
int main(int argc,char **argv) {
    if (argc==3 && !strcmp(argv[1],"--fuzz")) {
        int rounds=atoi(argv[2]);
        fuzz(rounds);vt_free(&v);
        printf("PASS: %d VT fuzz rounds, %u checks (invariants after random input, resizes, resets)\n",rounds,checks);
        return 0;
    }
    basic();scrolling();alternate_test();colours();unicode_test();resize_test();strings_and_keys();replies_test();
    mouse_test();conceal();style_pressure();split_and_fuzz();
    vt_free(&v);
    printf("PASS: %u VT checks: cursor/editing, margins/scrollback, alternate screens, SGR, palette, Unicode, reflow, "
           "replies, OSC, mouse, keys, style pressure and parser bounds\n",checks);
}
