#include "vt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct vt v,other;
static char replies[1024];static size_t reply_length;
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort();}}while(0)
static void reply_cb(void *unused,const char *bytes,size_t n) {(void)unused;CHECK(reply_length+n<sizeof replies);memcpy(replies+reply_length,bytes,n);reply_length+=n;replies[reply_length]=0;}
static void start(int cols,int rows) {vt_init(&v,cols,rows,reply_cb,NULL);reply_length=0;replies[0]=0;}
static void feed(const char *s) {vt_feed(&v,s,strlen(s));}
static const struct vt_cell *cell(int y,int x) {return vt_view_cell(&v,y,x,0);}
static void text(int row,const char *expected) {char s[VT_COLS*12+1];vt_line_text(&v,row,0,s,sizeof s);CHECK(!strcmp(s,expected));}
static void invariant(const struct vt *t) {
    CHECK(t->cols>=2 && t->cols<=VT_COLS && t->rows>=2 && t->rows<=VT_ROWS);
    CHECK(t->history_count<=VT_HISTORY && t->history_head<VT_HISTORY);
    CHECK(t->nparam<=16 && t->string_length<sizeof t->string && t->utf8_left<=3);
    const struct vt_screen *screens[2]={&t->primary,&t->alternate};
    for(int n=0;n<2;n++) {
        const struct vt_screen *s=screens[n];bool used[VT_ROWS]={0};
        CHECK(s->cursor.x>=0 && s->cursor.x<t->cols && s->cursor.y>=0 && s->cursor.y<t->rows);
        CHECK(s->top>=0 && s->top<s->bottom && s->bottom<t->rows);
        for(int y=0;y<VT_ROWS;y++){CHECK(s->order[y]<VT_ROWS && !used[s->order[y]]);used[s->order[y]]=true;}
        for(int y=0;y<t->rows;y++)for(int x=0;x<t->cols;x++) {
            const struct vt_cell *c=&s->lines[s->order[y]].cells[x];CHECK(c->width<=2);
            if(c->width==2)CHECK(x+1<t->cols && s->lines[s->order[y]].cells[x+1].width==0);
            if(!c->width)CHECK(x>0 && s->lines[s->order[y]].cells[x-1].width==2);
        }
    }
}
static void basic(void) {
    start(10,4);feed("abcdef\rXY");text(0,"XYcdef");feed("\bZ");text(0,"XZcdef");
    feed("\033[3;4H!");CHECK(v.primary.cursor.x==4 && v.primary.cursor.y==2);CHECK(cell(2,3)->rune=='!');
    feed("\033[999999999999A\033[999999999999D");CHECK(v.primary.cursor.x==0 && v.primary.cursor.y==0);
    feed("\033[999999999999B\033[999999999999C");CHECK(v.primary.cursor.x==9 && v.primary.cursor.y==3);
    feed("\033[2J");CHECK(v.primary.cursor.x==9 && v.primary.cursor.y==3);text(0,"");
    feed("\033[HabcdefghijK");text(0,"abcdefghij");text(1,"K");
    feed("\033[?7l\033[1;10HXY");text(0,"abcdefghiY");CHECK(v.primary.cursor.x==9);
    feed("\033[?7h\033[H\033[2Jabcdef\033[1;3H\033[2@");text(0,"ab  cdef");
    feed("\033[2P");text(0,"abcdef");feed("\033[2X");text(0,"ab  ef");
    feed("\033[1K");text(0,"    ef");feed("\033[2K");text(0,"");
    feed("\033[H\033[32mA\0337\033[4;9H\033[31mB\0338C");text(0,"AC");CHECK(cell(0,1)->fg==vt_color(2));
    feed("\033[6n\033[5n\033[c");CHECK(!strcmp(replies,"\033[1;3R\033[0n\033[?1;2c"));
    start(20,3);feed("a\tb");CHECK(cell(0,8)->rune=='b');feed("\033[3g\r\t");CHECK(v.primary.cursor.x==19);
    feed("\033[1;5H\033H\r\t");CHECK(v.primary.cursor.x==4);feed("\033[Z");CHECK(v.primary.cursor.x==0);
}
static void scrolling(void) {
    start(6,5);feed("a\r\nb\r\nc\r\nd\r\ne");
    feed("\033[2;4r\033[4;1H\n");text(0,"a");text(1,"c");text(2,"d");text(3,"");text(4,"e");CHECK(v.history_count==0);
    feed("\033[2;1H\033M");text(1,"");text(2,"c");text(3,"d");
    feed("\033[?6h\033[1;1HX\033[999;1HY");CHECK(cell(1,0)->rune=='X' && cell(3,0)->rune=='Y');
    feed("\033[?6n");CHECK(!strcmp(replies,"\033[?3;2R"));feed("\033[?6l\033[r");
    feed("\033[2;1H\033[L");text(1,"");text(2,"X");feed("\033[M");text(1,"X");
    start(4,2);feed("1\r\n2\r\n3");CHECK(v.history_count==1);text(0,"2");text(1,"3");
    char s[40];vt_line_text(&v,0,1,s,sizeof s);CHECK(!strcmp(s,"1"));
    for(int i=0;i<350;i++)feed("\r\nx");
    CHECK(v.history_count==300);
    feed("\033[3J");CHECK(v.history_count==0);text(1,"x");
}
static void alternate_test(void) {
    start(12,4);feed("primary\033[32m\033[?1049h\033[2J\033[Halt");CHECK(v.alt);text(0,"alt");
    feed("\033[?1049hX");text(0,"altX");
    for(int i=0;i<20;i++)feed("\r\nsecondary");
    CHECK(v.history_count==0);
    feed("\033[?25l\033[?1h\033[?2004h\033[5 q");CHECK(!v.cursor_visible && v.application_cursor && v.bracketed_paste && v.cursor_style==5);
    feed("\033[?1049lZ");CHECK(!v.alt);text(0,"primaryZ");CHECK(cell(0,7)->fg==vt_color(2));
    feed("\033[?1049l");text(0,"primaryZ");feed("\033[?1049h");text(0,"");feed("\033[?1049l");text(0,"primaryZ");
}
static void colours(void) {
    start(20,3);feed("\033[1;2;3;4;7;8;9;38;5;196;48;2;12;34;56mX");
    CHECK(cell(0,0)->fg==0xff0000 && cell(0,0)->bg==0x0c2238 && cell(0,0)->flags==127);
    feed("\033[22;23;24;27;28;29;39;49mY");CHECK(!cell(0,1)->flags && cell(0,1)->fg==VT_DEFAULT && cell(0,1)->bg==VT_DEFAULT);
    feed("\033[94;103mZ");CHECK(cell(0,2)->fg==vt_color(12) && cell(0,2)->bg==vt_color(11));
    feed("\033[0;38;2;2mW");CHECK(cell(0,3)->fg==VT_DEFAULT && cell(0,3)->flags==0);
    feed("\033[48;5;21m\033[K");CHECK(cell(0,9)->bg==vt_color(21));
}
static void unicode_test(void) {
    start(6,3);feed("caf\xc3");feed("\xa9 e\xcc");feed("\x81");text(0,"caf\xc3\xa9 e\xcc\x81");
    CHECK(cell(0,5)->rune=='e' && cell(0,5)->combining[0]==0x301);
    feed("\xe7\x95\x8c");CHECK(cell(1,0)->rune==0x754c && cell(1,0)->width==2 && !cell(1,1)->width);
    feed("\033[2;2HX");CHECK(cell(1,0)->rune==' ' && cell(1,0)->width==1 && cell(1,1)->rune=='X');
    feed("\033[2;1H\xe7\x95\x8c\033[2;1H\033[P");invariant(&v);
    start(4,3);feed("abc\xe7\x95\x8c");text(0,"abc");CHECK(cell(1,0)->width==2);
    feed("\033[?7l\033[1;4H\xe7\x95\x8c");CHECK(cell(0,3)->rune==0xfffd && cell(0,3)->width==1);
    start(12,3);feed("\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80");CHECK(cell(0,0)->rune==0xfffd && cell(0,1)->rune==0xfffd && cell(0,2)->rune==0xfffd && cell(0,3)->rune==0xfffd);
    feed("\033(0lqqk\033(B");CHECK(cell(0,4)->rune==0x250c && cell(0,5)->rune==0x2500 && cell(0,7)->rune==0x2510);
}
static void resize_test(void) {
    start(4,3);feed("abc\xe7\x95\x8c");CHECK(vt_resize(&v,8,3));text(0,"abc\xe7\x95\x8c");
    CHECK(v.primary.cursor.x==5 && v.primary.cursor.y==0);
    start(5,4);feed("abcdefghij");CHECK(v.primary.cursor.wrap);CHECK(vt_resize(&v,10,4));text(0,"abcdefghij");
    CHECK(v.primary.cursor.wrap);feed("K");text(1,"K");
    CHECK(vt_resize(&v,4,4));text(0,"abcd");text(1,"efgh");text(2,"ijK");
    feed("\033[?1049h\033[Halternate");CHECK(vt_resize(&v,8,5));CHECK(v.alt);feed("\033[?1049l");text(0,"abcdefgh");text(1,"ijK");
    start(6,5);feed("a\r\nb\r\nc\r\nd\r\ne");CHECK(vt_resize(&v,6,3));text(0,"c");text(2,"e");CHECK(v.history_count==2);
    CHECK(vt_resize(&v,6,6));text(0,"a");text(4,"e");
    start(8,4);feed("\033[3;4H\xe7\x95\x8c");CHECK(vt_resize(&v,4,3));invariant(&v);
}
static void strings_and_keys(void) {
    start(20,4);feed("\033]2;Device log\007X\033Pignored text\033\\Y\033]8;;https://invalid.example\033\\Z");text(0,"XYZ");CHECK(!strcmp(v.title,"Device log"));
    feed("\033[999999999999999;99999999999999H!");CHECK(cell(3,19)->rune=='!');
    feed("\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17H");invariant(&v);
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
    for(unsigned i=0;i<sizeof keys/sizeof keys[0];i++) {
        n=vt_key_sequence(&v,keys[i].key,keys[i].mods,b);
        CHECK(n==strlen(keys[i].want) && !memcmp(b,keys[i].want,n));
    }
    feed("\033=");CHECK(v.keypad_application);
    n=vt_key_sequence(&v,VT_KEY_KP_0+5,0,b);CHECK(n==3 && !memcmp(b,"\033Ou",3));
    n=vt_key_sequence(&v,VT_KEY_KP_ENTER,0,b);CHECK(n==3 && !memcmp(b,"\033OM",3));
    n=vt_key_sequence(&v,VT_KEY_KP_SLASH,0,b);CHECK(n==3 && !memcmp(b,"\033Oo",3));
    feed("\033>");CHECK(!v.keypad_application);
    v.backspace_sends_bs=true;n=vt_key_sequence(&v,VT_KEY_BACKSPACE,0,b);CHECK(n==1 && b[0]==8);
    n=vt_key_sequence(&v,VT_KEY_BACKSPACE,4,b);CHECK(n==1 && b[0]==127);
    feed("\033c");CHECK(v.backspace_sends_bs && v.alt_sends_escape);   /* settings survive a reset */
    v.backspace_sends_bs=false;v.alt_sends_escape=false;n=vt_key_sequence(&v,'x',2,b);CHECK(n==1 && b[0]=='x');
    v.alt_sends_escape=true;
    feed("\033]133;A;cwd=/home/tester\007");CHECK(v.shell_ready && !strcmp(v.directory,"/home/tester"));
    feed("\033]133;C\007");CHECK(!v.shell_ready && v.prompt_serial==2);
    feed("\033c");CHECK(!v.alt && !v.application_cursor && v.cursor_visible && !v.bracketed_paste && !v.history_count);text(0,"");
}
static void conceal(void) {
    start(20,3);feed("\033[8msecret\033[28m ok");text(0,"       ok");
}

static uint32_t random_state=0x532985f;
static unsigned random_next(void) {random_state=random_state*1664525u+1013904223u;return random_state;}
static void split_and_fuzz(void) {
    const char *sample="hello\r\n\033[?1049h\033[H\033[38;2;1;2;3m\xe7\x95\x8c\xcc\x81\033[2;2H!\033]2;Title\033\\\033[?1049lworld";
    start(30,8);vt_feed(&v,sample,strlen(sample));vt_init(&other,30,8,reply_cb,NULL);
    for(size_t i=0;i<strlen(sample);i++)vt_feed(&other,sample+i,1);
    CHECK(!memcmp(&v,&other,sizeof v));
    for(int round=0;round<1500;round++) {
        unsigned char bytes[80];for(unsigned j=0;j<sizeof bytes;j++)bytes[j]=(unsigned char)(random_next()>>16);
        vt_feed(&v,bytes,sizeof bytes);invariant(&v);
        if(round%29==0) {CHECK(vt_resize(&v,2+(int)(random_next()%VT_COLS),2+(int)(random_next()%VT_ROWS)));invariant(&v);}
        if(round%101==0)vt_reset(&v);
    }
}
int main(void) {
    basic();scrolling();alternate_test();colours();unicode_test();resize_test();strings_and_keys();conceal();split_and_fuzz();
    printf("PASS: %u VT checks: cursor/editing, margins/history, alternate screens, SGR, Unicode, resize, replies, key modes and parser bounds\n",checks);
}
