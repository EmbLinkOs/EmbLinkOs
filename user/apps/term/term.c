/* EmbLink Terminal: a private PTY session, and a screen for whatever runs in it.
 *
 * The shells edit their own lines (shell/edit); this is a terminal, not a line
 * editor. Keys are encoded as xterm encodes them and written to the PTY master;
 * output goes through the screen model (user/terminal/vt.h) and is painted.
 * The shells mark their prompts with OSC 133, which gives the window its title
 * and lets a resize redraw a prompt cleanly. The master is nonblocking and
 * input is queued so a slow child cannot freeze the window. Closing the master
 * hangs up the slave, including blocked reads.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "../../terminal/vt.h"
#include "../../terminal/render.h"
#include "term_probe.h"
#include "font.h"
#include "backend.h"

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

/* The cell model owns the screen; rendering never changes parsed content. */
static struct vt g_vt;
static int g_cols=80,g_rows=20,g_view=0,g_master=-1,g_shell=-1;
#define FD_SHELL_IN g_master
#define FD_SHELL_OUT g_master
static bool g_dead=false,g_at_prompt=false;
/* A resize happened while the program wanted in-band size reports (and the
 * prompt was erased for it): report once the kernel knows the size. A resize
 * from before reports were on is not reported -- the program learned the size
 * when it turned them on, and nothing was erased for it to redraw. */
static bool g_size_report=false;
static char g_notice[96];
static unsigned char g_pending[65568];
static size_t g_pending_n;
static void term_flush(void) {
    if (!g_pending_n || g_dead) return;
    int64_t written=embk_write(g_master,g_pending,g_pending_n);
    if (written>0) {g_pending_n-=(size_t)written;memmove(g_pending,g_pending+written,g_pending_n);}
}
/* Queued, then written at once: a key reaches the program in the same loop
 * iteration it was pressed, not on the next idle pass. */
static bool term_send(const void *data,size_t n) {
    if (n>sizeof g_pending-g_pending_n) {snprintf(g_notice,sizeof g_notice,"Input queue is full; wait for the program to read.");return false;}
    memcpy(g_pending+g_pending_n,data,n);g_pending_n+=n;term_flush();return true;
}
static void term_reply(void *unused,const char *data,size_t n) {(void)unused;(void)term_send(data,n);}

/* The directory the shell's last prompt mark named (OSC 133;A;cwd=), or the
 * last OSC 7 a program sent. */
static char g_cwd[192]="/";

/* The window's title is the WORKING DIRECTORY, the way a real terminal titles
 * itself. "Terminal" tells you nothing you did not already know from the icon;
 * where the shell is standing is the one fact worth carrying in the chrome,
 * and it is the fact you want when three of these are open at once. */
static const char *title_text(void) {
    static char t[224];
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl && strncmp(g_cwd, home, hl) == 0 && (g_cwd[hl] == '/' || g_cwd[hl] == 0))
        snprintf(t, sizeof t, "~%s", g_cwd + hl);
    else
        snprintf(t, sizeof t, "%s", g_cwd);
    return t;
}

/* Clear: the scrollback and the screen. A shell at its prompt is asked to
 * redraw it (Ctrl+L) rather than having it wiped from under its editor. */
static void term_clear(void) {
    g_view=0;
    if (g_at_prompt && g_vt.edit_shell && !g_dead) {vt_clear_history(&g_vt);(void)term_send("\f",1);}
    else vt_feed(&g_vt,"\033[2J\033[H\033[3J",11);
}
static void term_say(const char *s) {
    for (;*s;s++) vt_feed(&g_vt,*s=='\n'?"\r\n":s,*s=='\n'?2:1);
}

/* --- the test probe ----------------------------------------------------------
 * What the window shows, kept current for the acceptance tools (term_probe.h).
 * Not static: tools find it by symbol, and a static that is only ever written
 * is a store the compiler may drop. */
__attribute__((used)) struct term_probe g_term_probe;
static uint32_t g_frames;

static size_t probe_line(const struct vt_cell *cells,char *dst,size_t cap) {
    size_t n=0;int last=-1;
    for (int x=0;x<g_vt.cols;x++) {
        const struct vt_cell *c=&cells[x];
        if (c->width && (c->cp!=' ' || c->flags) && !(vt_style_of(&g_vt,c)->attrs&VT_HIDDEN)) last=x;
    }
    for (int x=0;x<=last;x++) {
        const struct vt_cell *c=&cells[x];
        if (!c->width) continue;
        char b[64];size_t k=vt_style_of(&g_vt,c)->attrs&VT_HIDDEN?(b[0]=' ',1):vt_cell_text(&g_vt,c,b);
        if (n+k>cap) return n;
        memcpy(dst+n,b,k);n+=k;
    }
    return n;
}
static uint32_t probe_screen(const struct vt_screen *s,char *dst) {
    size_t n=0;
    for (int y=0;y<g_vt.rows;y++) {
        if (y) {if (n+1>=TERM_PROBE_TEXT) break;dst[n++]='\n';}
        n+=probe_line(s->rows[y]->cells,dst+n,TERM_PROBE_TEXT-1-n);
    }
    dst[n]=0;return (uint32_t)n;
}
static uint8_t probe_flags(const struct vt_style *s) {
    return (uint8_t)(((s->attrs&VT_BOLD)?TERM_PROBE_CELL_BOLD:0)|((s->attrs&VT_DIM)?TERM_PROBE_CELL_DIM:0)|
                     ((s->attrs&VT_ITALIC)?TERM_PROBE_CELL_ITALIC:0)|(s->underline?TERM_PROBE_CELL_UNDERLINE:0)|
                     ((s->attrs&VT_INVERSE)?TERM_PROBE_CELL_INVERSE:0)|((s->attrs&VT_HIDDEN)?TERM_PROBE_CELL_HIDDEN:0)|
                     ((s->attrs&VT_STRIKE)?TERM_PROBE_CELL_STRIKE:0)|((s->attrs&(VT_BLINK|VT_RAPID))?TERM_PROBE_CELL_BLINK:0));
}
static void probe_refresh(void) {
    struct term_probe *p=&g_term_probe;
    __atomic_store_n(&p->serial,p->serial+1,__ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const struct vt_screen *s=g_vt.alt?&g_vt.alternate:&g_vt.primary;
    p->magic=TERM_PROBE_MAGIC;p->version=TERM_PROBE_VERSION;p->size=sizeof *p;
    p->cols=g_vt.cols;p->rows=g_vt.rows;p->cursor_x=s->cursor.x;p->cursor_y=s->cursor.y;
    p->at_prompt=g_at_prompt;p->dead=g_dead;p->alt=g_vt.alt;p->cursor_visible=g_vt.cursor_visible;
    p->view=g_view;p->shell=g_shell;p->master=g_master;p->session=0;p->sessions=1;
    p->prompt_serial=g_vt.prompt_serial;p->history_count=g_vt.history_rows;p->frames=g_frames;
    p->input_len=0;p->caret=0;p->input[0]=0;      /* no local line any more: the shells edit their own */
    p->flags=(g_vt.edit_shell?TERM_PROBE_EDIT_SHELL:0)|(g_vt.inband_resize?TERM_PROBE_INBAND_SIZE:0);
    snprintf(p->cwd,sizeof p->cwd,"%s",g_cwd);
    p->text_len=probe_screen(s,p->text);p->primary_len=probe_screen(&g_vt.primary,p->primary);
    size_t cells=0;
    for (int y=0;y<g_vt.rows;y++) for (int x=0;x<g_vt.cols && cells<TERM_PROBE_CELLS;x++,cells++) {
        const struct vt_cell *c=&s->rows[y]->cells[x];
        const struct vt_style *st=vt_style_of(&g_vt,c);
        uint32_t base=c->flags&VT_CELL_CLUSTER?g_vt.clusters[c->cp][0]:c->cp;
        p->cells[cells]=(struct term_probe_cell){base,
            st->fg==VT_COLOR_DEFAULT?TERM_PROBE_DEFAULT:vt_rgb(&g_vt,st->fg,true),
            st->bg==VT_COLOR_DEFAULT?TERM_PROBE_DEFAULT:vt_rgb(&g_vt,st->bg,false),
            c->width,probe_flags(st),{0,0}};
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&p->serial,p->serial+1,__ATOMIC_RELEASE);
}

/* --- runtime hooks -------------------------------------------------------- */

/* Whether the window has the keyboard: the cursor goes hollow without it, and
 * programs that asked (DECSET 1004) are told. */
static bool g_focused=true;
static uint64_t g_activity;      /* the last key or output: a blinking cursor shows from here */
static bool cursor_blinks(void);
static bool cursor_phase(uint64_t now);
static bool text_phase(uint64_t now);
static struct term_render g_render;
static void term_focus(void) {
    static uint64_t next;static int focused=-1;
    uint64_t now=embk_uptime_ms();
    if (now<next) return;
    next=now+100;
    int f=embk_win_focused();
    if (f==focused) return;
    focused=f;g_focused=f!=0;g_activity=now;em_request_frame();
    char out[4];size_t n=vt_focus_report(&g_vt,f!=0,out);
    if (n && !g_dead) (void)term_send(out,n);
}

/* idle: drain the terminal master without blocking; wake the renderer
 * only when something actually arrived. Also notice the shell dying. */
static void term_idle(void) {
    bool got = false;
    term_flush();
    term_focus();
    static int last_cols=0,last_rows=0;
    if (g_cols!=last_cols || g_rows!=last_rows) {
        struct embk_terminal_size size={(uint16_t)g_rows,(uint16_t)g_cols,0,0};
        if (embk_terminal_control(g_master,EMBK_TERMINAL_SET_SIZE,&size,sizeof size)==0) {
            last_cols=g_cols; last_rows=g_rows;
            if (g_size_report && g_vt.inband_resize) {char r[48];size_t n=vt_size_report(&g_vt,r);(void)term_send(r,n);}
            g_size_report=false;
        }
    }
    size_t budget=16384;
    while (budget) {
        int64_t n = embk_fd_avail(FD_SHELL_OUT);
        if (n <= 0) break;
        char buf[256];
        size_t want=n<(int64_t)sizeof buf?(size_t)n:sizeof buf;
        if (want>budget) want=budget;
        int r = (int)embk_read(FD_SHELL_OUT, buf, want);
        if (r <= 0) break;
        uint64_t scrolled=g_vt.scrolls;
        vt_feed(&g_vt,buf,(size_t)r);
        /* reading the scrollback, the view stays on what it shows */
        if (g_view>0) {g_view+=(int)(g_vt.scrolls-scrolled);if (g_view>(int)g_vt.history_rows) g_view=(int)g_vt.history_rows;}
        got = true; budget-=(size_t)r;
        g_activity=embk_uptime_ms();
    }
    static unsigned prompt_serial;
    if (prompt_serial!=g_vt.prompt_serial) {
        prompt_serial=g_vt.prompt_serial;g_at_prompt=g_vt.shell_ready;got=true;
    }
    static char directory[sizeof g_vt.directory];
    if (strcmp(directory,g_vt.directory)) {
        memcpy(directory,g_vt.directory,sizeof directory);
        if (directory[0]) snprintf(g_cwd,sizeof g_cwd,"%.*s",(int)sizeof g_cwd-1,directory);
        got=true;
    }
    /* OSC 52, when allowed: the program's text becomes the clipboard */
    if (g_vt.clipboard_pending) {
        g_vt.clipboard_pending=false;
        if (embk_clip_set(g_vt.clipboard,g_vt.clipboard_len)>=0) snprintf(g_notice,sizeof g_notice,"Copied by the program");
        got=true;
    }
    if (!g_dead && g_shell >= 0 && !embk_proc_alive(g_shell)) {
        g_dead = true;
        term_say("\n[shell exited -- press ESC to close]\n");
        got = true;
    }
    /* blinking text and a blinking cursor: a frame when a phase turns */
    uint64_t now=embk_uptime_ms();
    static int phase=-1;
    int p=(g_render.blink_text && text_phase(now) ? 1 : 0)|(cursor_blinks() && cursor_phase(now) ? 2 : 0);
    if (p!=phase) {phase=p;em_request_frame();}
    if (got) {probe_refresh();em_request_frame();}
}

static void term_copy(void) {
    static char copy[65536];size_t n=0;
    for (int y=0;y<g_rows;y++) {
        static char row[VT_MAX_COLS*12+1];size_t k=vt_line_text(&g_vt,y,(unsigned)g_view,row,sizeof row);
        if (n+k+1>sizeof copy) {snprintf(g_notice,sizeof g_notice,"Screen text exceeds the clipboard limit.");em_request_frame();return;}
        memcpy(copy+n,row,k);n+=k;
        if (y+1<g_rows && !vt_view_wrapped(&g_vt,y,(unsigned)g_view)) copy[n++]='\n';
    }
    if (embk_clip_set(copy,n)<0) snprintf(g_notice,sizeof g_notice,"Screen text could not be copied.");
    else snprintf(g_notice,sizeof g_notice,"Screen copied");
    em_request_frame();
}
static void term_paste(void) {
    if (g_dead) return;
    static char clipboard[65536],paste[65568];
    int64_t held=embk_clip_get(clipboard,sizeof clipboard);
    if (held<=0) return;
    if (held>(int64_t)sizeof clipboard) {snprintf(g_notice,sizeof g_notice,"Clipboard text is too large to paste.");em_request_frame();return;}
    /* Bracketed when the program asked (the shells do, at their prompts), so
     * the text arrives as text; otherwise a line break is Enter, as Terminal.app
     * pastes. Control bytes other than tab and newline are dropped. */
    bool bracketed=g_vt.bracketed_paste;
    size_t n=0;if (bracketed) {memcpy(paste,"\033[200~",6);n=6;}
    for (int64_t i=0;i<held;i++) {
        unsigned char c=(unsigned char)clipboard[i];
        if (c=='\r') {if (i+1<held && clipboard[i+1]=='\n') continue;c='\n';}
        if (c==127 || (c<32 && c!='\n' && c!='\t')) continue;
        if (!bracketed && c=='\n') c='\r';
        paste[n++]=(char)c;
    }
    if (bracketed) {memcpy(paste+n,"\033[201~",6);n+=6;}
    (void)term_send(paste,n);
    em_request_frame();
}

/* --- keys ---------------------------------------------------------------------
 * The kernel queue says which key produced each byte (em_key_code): Up is not
 * Ctrl+S here although both are 0x13, Backspace is not Ctrl+H, and F5 arrives
 * at all. key_identity() names the key the way the screen model's encoder does,
 * or returns 0 for a byte that is simply text. */
static unsigned key_identity(int c,unsigned code,unsigned flags) {
    if (c&EM_KEY_ONLY) {
        code=(unsigned)c&0xffff;
        if (code>=EMBK_KC_F1 && code<=EMBK_KC_F12) return VT_KEY_F1+(code-EMBK_KC_F1);
        return code==EMBK_KC_INS?VT_KEY_INSERT:code==EMBK_KC_MENU?VT_KEY_MENU:0;
    }
    switch (code) {
    case EMBK_KC_UP: return VT_KEY_UP;
    case EMBK_KC_DOWN: return VT_KEY_DOWN;
    case EMBK_KC_LEFT: return VT_KEY_LEFT;
    case EMBK_KC_RIGHT: return VT_KEY_RIGHT;
    case EMBK_KC_HOME: return VT_KEY_HOME;
    case EMBK_KC_END: return VT_KEY_END;
    case EMBK_KC_PGUP: return VT_KEY_PAGEUP;
    case EMBK_KC_PGDN: return VT_KEY_PAGEDOWN;
    case EMBK_KC_DEL: return VT_KEY_DELETE;
    }
    if (flags&EMBK_KEYF_KEYPAD) {
        if (code>='0' && code<='9') return VT_KEY_KP_0+(code-'0');
        switch (code) {
        case '.': return VT_KEY_KP_DOT;
        case '\n': return VT_KEY_KP_ENTER;
        case '+': return VT_KEY_KP_PLUS;
        case '-': return VT_KEY_KP_MINUS;
        case '*': return VT_KEY_KP_STAR;
        case '/': return VT_KEY_KP_SLASH;
        }
    }
    switch (code) {
    case '\b': return VT_KEY_BACKSPACE;
    case '\n': return VT_KEY_ENTER;
    case 27: return VT_KEY_ESCAPE;
    case '\t': return VT_KEY_TAB;
    }
    return 0;
}
/* Ctrl with a key that is not a letter: the C0 bytes xterm sends for them. */
static int control_byte(unsigned code) {
    switch (code) {
    case ' ': case '2': case '@': return 0;
    case '[': case '3': return 27;
    case '\\': case '4': return 28;
    case ']': case '5': return 29;
    case '^': case '6': return 30;
    case '-': case '_': case '/': case '7': return 31;
    case '8': case '?': return 127;
    }
    return -1;
}
static unsigned vt_mods(unsigned mods) {
    return ((mods&EMBK_KM_SHIFT)?1:0)|((mods&EMBK_KM_ALT)?2:0)|((mods&EMBK_KM_CTRL)?4:0);
}
static size_t encode_key(int c,unsigned key,unsigned code,unsigned mods,char out[16]) {
    unsigned vtmods=vt_mods(mods);
    if (key) return vt_key_sequence(&g_vt,key,vtmods,out);
    if (c&EM_KEY_ONLY) return 0;
    if (!code) {out[0]=(char)c;return 1;}             /* a character's later UTF-8 byte */
    int byte=c;
    if ((mods&EMBK_KM_CTRL) && byte>=0x20 && byte<0x7f) {int ctl=control_byte(code);if (ctl>=0) byte=ctl;}
    return vt_key_sequence(&g_vt,(unsigned)byte,vtmods&2,out);
}
static void view_scroll(int rows) {
    if (g_vt.alt) return;
    g_view+=rows;
    if (g_view<0) g_view=0;
    if (g_view>(int)g_vt.history_rows) g_view=(int)g_vt.history_rows;
}

/* Keys are encoded for the program. Shift+PgUp/PgDn and the wheel page the
 * scrollback; GUI shortcuts never reach a program; once the shell has exited,
 * Escape closes the window. */
static int term_key(int c) {
    unsigned mods=(unsigned)em_key_modifiers(),code=(unsigned)em_key_code(),flags=(unsigned)em_key_flags();g_notice[0]=0;
    g_activity=embk_uptime_ms();
    unsigned key=key_identity(c,code,flags);
    if (c==EMBK_KEY_COPY || (c==3 && (mods&(EMBK_KM_CTRL|EMBK_KM_SHIFT))==(EMBK_KM_CTRL|EMBK_KM_SHIFT))) {term_copy();return 1;}
    if (c==EMBK_KEY_PASTE || (c==22 && (mods&EMBK_KM_SHIFT))) {term_paste();return 1;}
    if ((key==VT_KEY_PAGEUP || key==VT_KEY_PAGEDOWN) && (mods&EMBK_KM_SHIFT)) {
        view_scroll((key==VT_KEY_PAGEUP?1:-1)*(g_rows/2));
        em_request_frame();return 1;
    }
    if (mods&EMBK_KM_GUI) return 1;
    if (g_dead) return key==VT_KEY_ESCAPE?0:1;      /* 0: the runtime closes the window */
    char sequence[16];size_t n=encode_key(c,key,code,mods,sequence);
    if (n) (void)term_send(sequence,n);
    if (g_view) g_view=0;
    em_request_frame();
    return 1;                                    /* never reaches the toolkit */
}

/* --- the mouse -------------------------------------------------------------
 * What the program asked for (DECSET 9/1000/1002/1003) is reported in the
 * encoding it chose; Shift keeps the mouse for the terminal. With reporting
 * off the wheel pages the scrollback -- or, in the alternate screen (less,
 * man, vim), sends Up and Down, as Terminal.app does. The toolkit hands over
 * the pointer once a frame, so presses and releases are the button state's
 * edges and motion is a change of cell between frames. */
static unsigned g_buttons;
static int g_mouse_col=-1,g_mouse_row=-1;
static int g_cell_width,g_cell_height;
static void report(enum vt_mouse_event event,int button,int col,int row,int px,int py,unsigned mods) {
    char out[48];size_t n=vt_mouse_report(&g_vt,event,button,col,row,px,py,mods,out);
    if (n) (void)term_send(out,n);
}
static void term_mouse(float ox,float oy,float wheel) {
    float mx,my;ui_pointer_pos(&mx,&my);
    if (!g_cell_width || !g_cell_height) return;
    int px=(int)floorf(mx-ox),py=(int)floorf(my-oy);
    int col=px<0?-1:px/g_cell_width,row=py<0?-1:py/g_cell_height;
    bool inside=col>=0 && row>=0 && col<g_cols && row<g_rows;
    if (col<0) col=0;
    if (row<0) row=0;
    if (col>=g_cols) col=g_cols-1;
    if (row>=g_rows) row=g_rows-1;
    unsigned buttons=(unsigned)em_pointer_buttons(),keys=(unsigned)embk_key_mods();
    unsigned mods=vt_mods(keys);
    int notches=(int)lroundf(wheel);
    if (g_vt.mouse!=VT_MOUSE_OFF && !(keys&EMBK_KM_SHIFT) && !g_dead) {
        static const unsigned bit[3]={EMBK_MOUSE_LEFT,EMBK_MOUSE_MIDDLE,EMBK_MOUSE_RIGHT};
        for (int b=0;b<3;b++) {
            bool now=(buttons&bit[b])!=0,was=(g_buttons&bit[b])!=0;
            if (now && !was && inside) report(VT_MOUSE_PRESS,b,col,row,px,py,mods);
            else if (!now && was) report(VT_MOUSE_RELEASE,b,col,row,px,py,mods);
        }
        if ((col!=g_mouse_col || row!=g_mouse_row) && (inside || buttons)) {
            int held=buttons&EMBK_MOUSE_LEFT?0:buttons&EMBK_MOUSE_MIDDLE?1:buttons&EMBK_MOUSE_RIGHT?2:3;
            report(VT_MOUSE_MOTION,held,col,row,px,py,mods);
        }
        for (int k=notches>0?notches:-notches;k-- && inside;) report(VT_MOUSE_PRESS,notches>0?4:5,col,row,px,py,mods);
    } else if (notches && g_vt.alt && !g_dead) {
        char seq[16];size_t n=vt_key_sequence(&g_vt,notches>0?VT_KEY_UP:VT_KEY_DOWN,0,seq);
        for (int k=(notches>0?notches:-notches)*3;k--;) (void)term_send(seq,n);
    } else if (notches) view_scroll(notches*3);
    g_buttons=buttons;g_mouse_col=col;g_mouse_row=row;
}

/* --- spawn the shell on an isolated terminal slave ----------------------- */
static bool term_spawn_shell(void) {
    int pair[2];
    if (embk_openpty(pair)<0) return false;
    g_master=pair[0];
    struct embk_terminal_attr attr;
    if (embk_terminal_control(pair[1],EMBK_TERMINAL_GET_ATTR,&attr,sizeof attr)<0) goto failed;
    attr.lflag&=~(EMBK_TLF_ECHO|EMBK_TLF_ISIG);
    if (embk_terminal_control(pair[1],EMBK_TERMINAL_SET_ATTR,&attr,sizeof attr)<0) goto failed;
    if (embk_fcntl_set_nonblock(g_master,1)<0) goto failed;
    struct embk_spawn_file_action acts[3]; memset(acts,0,sizeof acts);
    for (int i=0;i<3;i++) {
        acts[i].kind=EMBK_SPAWN_ACTION_INHERIT_FD;
        acts[i].target_fd=i; acts[i].src_obj_handle=pair[1];
    }

    char *argv[] = { "/system/bin/shell.elf", NULL };
    /* Start the shell in the user's HOME (docs/USERSPACE.md §5): cwd is never
     * inherited, so the terminal -- the session spawner here -- NAMES it via PWD,
     * and the shell's crt0 seeds cwd from it. TERM says what this screen is:
     * xterm's sequences, 256 colours, and (COLORTERM) 24-bit colour too. */
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/";
    char home_env[160], pwd_env[160];
    snprintf(home_env, sizeof home_env, "HOME=%s", home);
    snprintf(pwd_env, sizeof pwd_env, "PWD=%s", home);
    char user_env[64];
    snprintf(user_env, sizeof user_env, "USER=%s",
             getenv("USER") ? getenv("USER") : "user");
    char *senv[] = { home_env, pwd_env, user_env, "TERM=xterm-256color", "COLORTERM=truecolor",
                     "TERM_PROGRAM=EmbLinkTerminal", "TERM_PROGRAM_VERSION=1.0", NULL };
    int64_t h = embk_spawn_env("/system/bin/shell.elf", argv, senv, acts, 3);

    close(pair[1]);
    if (h<0) { close(g_master); g_master=-1; return false; }
    g_shell=(int)h; return true;
failed:
    close(pair[0]); close(pair[1]); g_master=-1; return false;
}

/* --- the view -------------------------------------------------------------- */

/* The renderer (user/terminal/render.h) paints rows that changed into one
 * retained buffer, and only those rows go to the screen. */
static bool g_render_ready,g_bell_drawn;
static uint64_t g_sync_since,g_bell_until;
static unsigned g_bells_seen;
static int g_dmg_y0,g_dmg_y1;
#define CURSOR_BLINK_MS 530
#define TEXT_BLINK_MS   500

static uint32_t rgb(Color c) {
    return ((uint32_t)(c.r*255+.5f)<<16)|((uint32_t)(c.g*255+.5f)<<8)|(uint32_t)(c.b*255+.5f);
}
/* The faces: MesloLGS Nerd Font Mono, as the Mac's profile -- 14 px at the
 * standard interface size, following the interface's scale -- and DejaVu
 * after it for what Meslo lacks. */
static bool load_fonts(const struct ui_theme *t) {
    static float loaded;
    float size=14.0f*t->text_caption/12.0f;
    if (g_render_ready && size==loaded) return true;
    static const char *const paths[4]={"/system/fonts/meslo-nf-mono.ttf","/system/fonts/meslo-nf-mono-bold.ttf",
        "/system/fonts/meslo-nf-mono-italic.ttf","/system/fonts/meslo-nf-mono-bold-italic.ttf"};
    struct font *faces[4];
    for (int i=0;i<4;i++) faces[i]=font_for_handle(em_font(paths[i]));
    struct font *mono=font_for_handle(em_font("/system/fonts/mono.ttf")),*sans=font_for_handle(em_font("/system/fonts/font.ttf"));
    if (!faces[0]) faces[0]=mono;                 /* an image without Meslo still has a terminal */
    if (!faces[0]) return false;
    struct font *chain[2];int n=0;
    if (mono && mono!=faces[0]) chain[n++]=mono;
    if (sans) chain[n++]=sans;
    if (!term_render_fonts(&g_render,faces,chain,n,size)) return false;
    loaded=size;g_render_ready=true;
    return true;
}
/* The profile's colours: until Preferences exist (docs/TODO.md), the theme's
 * text and background with the built-in 256-colour palette. Applied to the
 * model, so text already on the screen follows a theme change. */
static void apply_palette(const struct ui_theme *t) {
    static uint32_t applied[3];static bool once;
    uint32_t want[3]={rgb(t->text),rgb(t->bg),rgb(t->accent)};
    if (once && !memcmp(applied,want,sizeof want)) return;
    struct vt_palette p;
    for (unsigned i=0;i<256;i++) p.color[i]=vt_default_color(i);
    p.fg=want[0];p.bg=want[1];p.cursor=want[2];
    vt_set_palette(&g_vt,&p);
    memcpy(applied,want,sizeof want);once=true;
}
/* DECSCUSR 1, 3, 5 blink; 0 (the default block) only with DECSET 12. */
static bool cursor_blinks(void) {
    int s=g_vt.cursor_style;
    return s==1 || s==3 || s==5 || (s==0 && g_vt.cursor_blink);
}
static bool cursor_phase(uint64_t now) {
    return !cursor_blinks() || !g_focused || ((now-g_activity)/CURSOR_BLINK_MS)%2==0;
}
static bool text_phase(uint64_t now) { return (now/TEXT_BLINK_MS)%2==0; }
static void bell_frame(void) {
    uint32_t a=vt_cursor_rgb(&g_vt);int w=g_render.width,h=g_render.height;
    for (int y=0;y<h;y++) {
        uint32_t *row=g_render.pixels+(size_t)y*w;
        for (int x=0;x<w;x++) if (y<2 || y>=h-2 || x<2 || x>=w-2) row[x]=0xff000000u|a;
    }
}
static bool screen_bitmap(void) {
    const struct ui_theme *t=ui_theme();
    if (!load_fonts(t)) return false;
    int cw=g_render.cell_w,ch=g_render.cell_h;
    int cols=(int)((em_viewport_width()-28)/cw),rows=(int)((em_viewport_height()-t->toolbar_height-20)/ch);
    if (cols<2) cols=2;
    if (cols>VT_MAX_COLS) cols=VT_MAX_COLS;
    if (rows<2) rows=2;
    if (rows>VT_MAX_ROWS) rows=VT_MAX_ROWS;
    if (cols!=g_render.cols || rows!=g_render.rows || !g_render.pixels) {
        bool erased=g_vt.inband_resize && vt_prompt_erase(&g_vt);   /* the shell redraws on the report */
        if (g_vt.inband_resize) g_size_report=true;
        if (!vt_resize(&g_vt,cols,rows)) return false;
        if (erased) vt_prompt_here(&g_vt);
        if (!term_render_resize(&g_render,cols,rows)) return false;
        g_vt.pixel_width=g_render.width;g_vt.pixel_height=g_render.height;
    }
    g_cols=cols;g_rows=rows;g_cell_width=cw;g_cell_height=ch;
    if (g_view>(int)g_vt.history_rows) g_view=(int)g_vt.history_rows;
    if (g_vt.alt) g_view=0;
    apply_palette(t);
    uint64_t now=embk_uptime_ms();
    g_dmg_y0=g_dmg_y1=0;
    /* DECSET 2026: a program drawing a frame in parts is shown the whole
     * frame -- held for at most 200 ms, so one that never ends it still shows */
    if (g_vt.sync_output) {
        if (!g_sync_since) g_sync_since=now?now:1;
        if (g_frames && now-g_sync_since<200) {em_request_frame();return true;}
    } else g_sync_since=0;
    if (g_vt.bells!=g_bells_seen) {g_bells_seen=g_vt.bells;g_bell_until=now+150;}
    bool bell=now<g_bell_until;
    if (bell) em_request_frame();
    if (bell!=g_bell_drawn) term_render_invalidate(&g_render);   /* the visual bell comes or goes */
    struct vt_cursor *cursor=&vt_active(&g_vt)->cursor;
    struct term_frame fr={
        .back=(unsigned)g_view,
        .cursor=!g_view && !g_dead && g_vt.cursor_visible && cursor_phase(now),
        .cursor_x=cursor->x,.cursor_y=cursor->y,.cursor_shape=g_vt.cursor_style,
        .cursor_hollow=!g_focused,.blink_on=text_phase(now),.bold_is_bright=false,
    };
    bool painted=term_render_frame(&g_render,&g_vt,&fr);
    if (bell) {bell_frame();g_dmg_y0=0;g_dmg_y1=g_render.height;}
    else if (painted) {g_dmg_y0=g_render.dmg_y0;g_dmg_y1=g_render.dmg_y1;}
    g_bell_drawn=bell;
    if (painted || bell) g_frames++;
    /* The probe follows what is shown, not the blink: a reader takes longer
     * than a blink phase, and a probe rewritten under it is never whole. */
    static uint64_t probed_revision;static int probed_view=-1;
    if (painted && (g_vt.revision!=probed_revision || g_view!=probed_view)) {
        probed_revision=g_vt.revision;probed_view=g_view;probe_refresh();
    }
    return true;
}
static void term_view(void) {
    const Color bg=ui_theme()->bg;
    Window("Terminal",.background=bg) {
        AppBar(g_notice[0]?g_notice:title_text(),.background=bg) {
            if (Button("Copy screen").ghost().clicked()) term_copy();
            if (Button("Paste").ghost().clicked()) term_paste();
            if (Button("Clear").ghost().clicked()) term_clear();
        }
        VStack(.spacing=0,.px=14,.pt=10,.pb=10,.align=Leading,.clip=1,.background=bg) {
            float wheel=ui_take_wheel();
            float bx=0,by=0,bw=0,bh=0;
            if (ui_open_rect(&bx,&by,&bw,&bh)) term_mouse(bx+14,by+10,wheel);   /* the image sits inside the padding */
            if (screen_bitmap()) {
                em_flush();
                int w=g_render.width,h=g_render.height;
                ui_image_sized_damage(0x5445524d,g_render.pixels,(uint32_t)w,(uint32_t)h,(float)w,(float)h,
                                      0,g_dmg_y0,w,g_dmg_y1-g_dmg_y0);
            } else Text("Terminal display could not be resized.").secondary();
        }
        Spacer();
    }
}

int main(void) {
    if (!vt_init(&g_vt,g_cols,g_rows,term_reply,NULL)) return 1;
    if (!term_spawn_shell()) {
        term_say("can't start /system/bin/shell.elf\n");
        g_dead = true;
    }
    probe_refresh();
    em_set_key_hook(term_key);
    em_key_hook_wants_key_only(1);              /* F1-F12, Insert, Menu */
    em_set_idle_hook(term_idle);

    static EmApp app = {
        .title  = "Terminal",
    .min_size = { 320, 240 },
        /* Smaller by default. 740x480 opened as a slab that dominated the
         * desktop; a terminal you have just opened should look like a tool you
         * reached for, not the thing you are now doing. It is resizable. */
        .size   = { 620, 380 },
        .theme  = Dark,
        .chrome = Chromeless,
        .resize = Resizable,
        .view   = term_view,
        .font   = "/system/fonts/mono.ttf",   /* DejaVu Sans Mono -- aligned table columns */
    };
    int rc = em_app_run(&app);

    /* Close the emulator endpoint before collecting the shell. A blocked
     * slave read sees EOF; no global console mode is involved. */
    if (g_shell >= 0) {
        close(FD_SHELL_IN);          /* an fd, so close() -- NOT a handle close */
        embk_wait(g_shell);          /* ...so this returns */
    }
    term_render_free(&g_render);
    vt_free(&g_vt);
    return rc;
}
