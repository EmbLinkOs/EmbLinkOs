/* EmbLink Terminal: sessions in tabs, and a screen for whatever runs in each.
 *
 * Every tab is a session: a private PTY, the shell on its slave side, and the
 * screen model (user/terminal/vt.h) its output goes through. The shells edit
 * their own lines (shell/edit); this is a terminal, not a line editor. Keys are
 * encoded as xterm encodes them and written to the shown tab's PTY master; the
 * renderer (user/terminal/render.h) paints the shown tab. The shells mark their
 * prompts with OSC 133, which titles the window and tab and lets a resize
 * redraw a prompt cleanly. Masters are nonblocking and input is queued, so a
 * slow child cannot freeze the window. Closing a master hangs up its slave,
 * including blocked reads; a closed tab's shell is reaped when it has gone.
 *
 * The mouse selects (a drag, a double click a word, a triple click a line,
 * Alt a rectangle, Shift extends) unless the program asked for the mouse;
 * Shift takes it back. Super+F finds in the scrollback, Super+click opens a
 * link, the right button opens a menu. docs/TERMINAL.md has the whole list.
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
#include "../../terminal/screentext.h"
#include "term_probe.h"
#include "font.h"

#include "embk.h"
#include "ui.h"
#include "kit.h"
#include "em.h"
#include "theme.h"

#define TABS_MAX TERM_PROBE_TABS

struct session {
    struct vt vt;
    int master, shell;              /* the PTY master; the shell (-1 once it is gone) */
    bool dead, at_prompt;
    bool size_report;               /* a resize the program wants reported in-band */
    int view;                       /* rows back into the scrollback */
    int cols, rows;                 /* the size the kernel was last told */
    unsigned prompt_serial;
    char directory[256];            /* the last directory the model reported */
    char cwd[192];                  /* where the shell is: OSC 133 cwd=, or OSC 7 */
    unsigned char pending[65568];   /* input the program has not taken yet */
    size_t pending_n;
    struct term_selection sel;
    bool sel_alt;                   /* the screen the selection was made on */
    int focus_reported;             /* -1 never, else the last DECSET 1004 report */
    bool spawned;                   /* its shell was started (the first waits for the window's size) */
};

static struct session *g_tab[TABS_MAX];
static int g_tabs,g_active;
#define S (g_tab[g_active])
static int g_reap[TABS_MAX*2],g_nreap;    /* shells of closed tabs, not yet gone */
static int g_cols=80,g_rows=20;
static char g_notice[96];
static struct term_render g_render;
static bool g_focused=true;
static uint64_t g_activity;          /* the last key or output: a blinking cursor shows from here */

/* --- one session's input ------------------------------------------------------ */

static void flush(struct session *s) {
    if (!s->pending_n || s->dead) return;
    int64_t written=embk_write(s->master,s->pending,s->pending_n);
    if (written>0) {s->pending_n-=(size_t)written;memmove(s->pending,s->pending+written,s->pending_n);}
}
/* Queued, then written at once: a key reaches the program in the same loop
 * iteration it was pressed, not on the next idle pass. */
static bool send(struct session *s,const void *data,size_t n) {
    if (n>sizeof s->pending-s->pending_n) {snprintf(g_notice,sizeof g_notice,"Input queue is full; wait for the program to read.");return false;}
    memcpy(s->pending+s->pending_n,data,n);s->pending_n+=n;flush(s);return true;
}
static void reply(void *context,const char *data,size_t n) {(void)send(context,data,n);}
static void say(struct session *s,const char *text) {
    for (;*text;text++) vt_feed(&s->vt,*text=='\n'?"\r\n":text,*text=='\n'?2:1);
}

/* --- titles ------------------------------------------------------------------------
 * The window's title is where the shell is standing, the way a real terminal
 * titles itself -- or what the program called itself (OSC 0/2). A tab says
 * the last part of it, so several fit. */
static const char *home_dir(void) { const char *h=getenv("HOME");return h && h[0]?h:"/"; }
static void tilde(const char *path,char *out,size_t cap) {
    const char *home=home_dir();size_t hl=strlen(home);
    if (hl>1 && !strncmp(path,home,hl) && (path[hl]=='/' || !path[hl])) snprintf(out,cap,"~%s",path+hl);
    else snprintf(out,cap,"%s",path);
}
static const char *window_title(const struct session *s) {
    static char t[256];
    if (s->vt.title[0]) snprintf(t,sizeof t,"%s",s->vt.title);
    else tilde(s->cwd,t,sizeof t);
    return t;
}
/* Text cut to fit, never in the middle of a UTF-8 character. */
static void cut(char *dst,size_t cap,const char *src) {
    size_t n=strlen(src);
    if (n>=cap) {n=cap-1;while (n && ((unsigned char)src[n]&0xc0)==0x80) n--;}
    memcpy(dst,src,n);dst[n]=0;
}
static const char *tab_title(const struct session *s) {
    static char t[TABS_MAX][64];static int next;
    char *out=t[next++%TABS_MAX];
    if (s->vt.title[0]) {cut(out,64,s->vt.title);return out;}
    char path[256],name[80];tilde(s->cwd,path,sizeof path);
    const char *last=strrchr(path,'/');
    cut(name,sizeof name-12,last && last[1]?last+1:path);
    if (s->dead) strcat(name," (exited)");
    cut(out,64,name);
    return out;
}

/* --- sessions ---------------------------------------------------------------------- */

static bool spawn_shell(struct session *s,const char *dir) {
    int pair[2];
    if (embk_openpty(pair)<0) return false;
    s->master=pair[0];
    struct embk_terminal_attr attr;
    if (embk_terminal_control(pair[1],EMBK_TERMINAL_GET_ATTR,&attr,sizeof attr)<0) goto failed;
    attr.lflag&=~(EMBK_TLF_ECHO|EMBK_TLF_ISIG);
    if (embk_terminal_control(pair[1],EMBK_TERMINAL_SET_ATTR,&attr,sizeof attr)<0) goto failed;
    if (embk_fcntl_set_nonblock(s->master,1)<0) goto failed;
    /* The size first: a shell that started before it was known would draw
     * its first prompt for some other width. */
    struct embk_terminal_size size={(uint16_t)g_rows,(uint16_t)g_cols,0,0};
    if (embk_terminal_control(s->master,EMBK_TERMINAL_SET_SIZE,&size,sizeof size)==0) {s->cols=g_cols;s->rows=g_rows;}
    struct embk_spawn_file_action acts[3];memset(acts,0,sizeof acts);
    for (int i=0;i<3;i++) {acts[i].kind=EMBK_SPAWN_ACTION_INHERIT_FD;acts[i].target_fd=i;acts[i].src_obj_handle=pair[1];}
    char *argv[]={"/system/bin/shell.elf",NULL};
    s->spawned=true;
    /* cwd is never inherited (docs/USERSPACE.md §5): the terminal NAMES it
     * via PWD -- the user's HOME, or for a new tab the directory the shown
     * tab is in -- and the shell's crt0 seeds cwd from it. TERM says what
     * this screen is: xterm's sequences, 256 colours, and (COLORTERM) 24-bit
     * colour too. */
    char home_env[160],pwd_env[224],user_env[64];
    snprintf(home_env,sizeof home_env,"HOME=%s",home_dir());
    snprintf(pwd_env,sizeof pwd_env,"PWD=%s",dir && dir[0]?dir:home_dir());
    snprintf(user_env,sizeof user_env,"USER=%s",getenv("USER")?getenv("USER"):"user");
    char *env[]={home_env,pwd_env,user_env,"TERM=xterm-256color","COLORTERM=truecolor",
                 "TERM_PROGRAM=EmbLinkTerminal","TERM_PROGRAM_VERSION=1.0",NULL};
    int64_t h=embk_spawn_env("/system/bin/shell.elf",argv,env,acts,3);
    close(pair[1]);
    if (h<0) {close(s->master);s->master=-1;return false;}
    s->shell=(int)h;
    return true;
failed:
    close(pair[0]);close(pair[1]);s->master=-1;return false;
}
static void apply_palette_to(struct vt *v);
static void start_shell(struct session *s) {
    if (s->spawned || s->dead) return;
    if (!spawn_shell(s,s->cwd)) {s->spawned=true;say(s,"can't start /system/bin/shell.elf\n");s->dead=true;}
}
static struct session *session_new(const char *dir) {
    struct session *s=calloc(1,sizeof *s);
    if (!s) return NULL;
    if (!vt_init(&s->vt,g_cols,g_rows,reply,s)) {free(s);return NULL;}
    apply_palette_to(&s->vt);
    s->vt.pixel_width=g_render.width;s->vt.pixel_height=g_render.height;
    s->master=s->shell=-1;s->focus_reported=-1;
    snprintf(s->cwd,sizeof s->cwd,"%s",dir && dir[0]?dir:home_dir());
    if (g_render.cols) start_shell(s);        /* the window's size is known; the first tab waits for it */
    return s;
}
static void probe_refresh(void);
static void tab_show(int i) {
    if (i<0 || i>=g_tabs) return;
    g_active=i;term_render_invalidate(&g_render);
    probe_refresh();em_request_frame();
}
static void tab_new(void) {
    if (g_tabs==TABS_MAX) {snprintf(g_notice,sizeof g_notice,"%d tabs is as many as a window holds.",TABS_MAX);return;}
    struct session *s=session_new(g_tabs?S->cwd:home_dir());
    if (!s) {snprintf(g_notice,sizeof g_notice,"No memory for another tab.");return;}
    g_tab[g_tabs]=s;
    tab_show(g_tabs++);
}
/* The tab goes, and its shell with it: the master closed is a hangup, and
 * the shell is reaped once it has gone. The last tab takes the window. */
static void tab_close(int i) {
    if (i<0 || i>=g_tabs) return;
    struct session *s=g_tab[i];
    if (s->master>=0) close(s->master);
    if (s->shell>=0 && g_nreap<(int)(sizeof g_reap/sizeof *g_reap)) g_reap[g_nreap++]=s->shell;
    vt_free(&s->vt);free(s);
    memmove(&g_tab[i],&g_tab[i+1],(size_t)(g_tabs-i-1)*sizeof *g_tab);
    g_tabs--;
    if (!g_tabs) {em_app_request_exit(0);return;}
    if (g_active>i || g_active==g_tabs) g_active--;
    tab_show(g_active);
}
static void reap(void) {
    for (int i=0;i<g_nreap;) {
        if (!embk_proc_alive(g_reap[i])) {embk_wait(g_reap[i]);g_reap[i]=g_reap[--g_nreap];}
        else i++;
    }
}

/* --- the test probe ----------------------------------------------------------
 * What the window shows, kept current for the acceptance tools (term_probe.h).
 * Not static: tools find it by symbol, and a static that is only ever written
 * is a store the compiler may drop. */
__attribute__((used)) struct term_probe g_term_probe;
static uint32_t g_frames;
static bool g_find_open,g_find_found;
static char g_find[128];
static struct term_match g_find_match;
static bool g_link_set;
static struct term_match g_link;
static char g_link_url[256];
static float g_font_px;
static int g_origin_x,g_origin_y;     /* where the screen's first cell is, in the window */

static size_t probe_line(const struct vt *v,const struct vt_cell *cells,char *dst,size_t cap) {
    size_t n=0;int last=-1;
    for (int x=0;x<v->cols;x++) {
        const struct vt_cell *c=&cells[x];
        if (c->width && (c->cp!=' ' || c->flags) && !(vt_style_of(v,c)->attrs&VT_HIDDEN)) last=x;
    }
    for (int x=0;x<=last;x++) {
        const struct vt_cell *c=&cells[x];
        if (!c->width) continue;
        char b[64];size_t k=vt_style_of(v,c)->attrs&VT_HIDDEN?(b[0]=' ',1):vt_cell_text(v,c,b);
        if (n+k>cap) return n;
        memcpy(dst+n,b,k);n+=k;
    }
    return n;
}
static uint32_t probe_screen(const struct vt *v,const struct vt_screen *s,char *dst) {
    size_t n=0;
    for (int y=0;y<v->rows;y++) {
        if (y) {if (n+1>=TERM_PROBE_TEXT) break;dst[n++]='\n';}
        n+=probe_line(v,s->rows[y]->cells,dst+n,TERM_PROBE_TEXT-1-n);
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
    if (!g_tabs) return;
    struct term_probe *p=&g_term_probe;
    struct session *se=S;const struct vt *v=&se->vt;
    __atomic_store_n(&p->serial,p->serial+1,__ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const struct vt_screen *s=v->alt?&v->alternate:&v->primary;
    p->magic=TERM_PROBE_MAGIC;p->version=TERM_PROBE_VERSION;p->size=sizeof *p;
    p->cols=v->cols;p->rows=v->rows;p->cursor_x=s->cursor.x;p->cursor_y=s->cursor.y;
    p->at_prompt=se->at_prompt;p->dead=se->dead;p->alt=v->alt;p->cursor_visible=v->cursor_visible;
    p->view=se->view;p->shell=se->shell;p->master=se->master;p->session=g_active;p->sessions=g_tabs;
    p->prompt_serial=v->prompt_serial;p->history_count=v->history_rows;p->frames=g_frames;
    p->input_len=0;p->caret=0;p->input[0]=0;      /* no local line any more: the shells edit their own */
    p->flags=(v->edit_shell?TERM_PROBE_EDIT_SHELL:0)|(v->inband_resize?TERM_PROBE_INBAND_SIZE:0);
    snprintf(p->cwd,sizeof p->cwd,"%s",se->cwd);
    p->text_len=probe_screen(v,s,p->text);p->primary_len=probe_screen(v,&v->primary,p->primary);
    size_t cells=0;
    for (int y=0;y<v->rows;y++) for (int x=0;x<v->cols && cells<TERM_PROBE_CELLS;x++,cells++) {
        const struct vt_cell *c=&s->rows[y]->cells[x];
        const struct vt_style *st=vt_style_of(v,c);
        uint32_t base=c->flags&VT_CELL_CLUSTER?v->clusters[c->cp][0]:c->cp;
        p->cells[cells]=(struct term_probe_cell){base,
            st->fg==VT_COLOR_DEFAULT?TERM_PROBE_DEFAULT:vt_rgb(v,st->fg,true),
            st->bg==VT_COLOR_DEFAULT?TERM_PROBE_DEFAULT:vt_rgb(v,st->bg,false),
            c->width,probe_flags(st),{0,0}};
    }
    p->tabs=(uint32_t)g_tabs;p->tab=(uint32_t)g_active;
    for (int i=0;i<TABS_MAX;i++) {
        p->tab_shell[i]=i<g_tabs && !g_tab[i]->dead?g_tab[i]->shell:-1;
        snprintf(p->tab_title[i],sizeof p->tab_title[i],"%s",i<g_tabs?tab_title(g_tab[i]):"");
    }
    p->selection_len=(uint32_t)term_select_text(&se->sel,v,p->selection,sizeof p->selection);
    p->find_open=g_find_open;p->find_found=g_find_open && g_find_found;
    p->find_row=p->find_found?vt_row_in_view(v,g_find_match.start.row,(unsigned)se->view):-1;
    p->find_col=p->find_found?g_find_match.start.col:-1;
    snprintf(p->find,sizeof p->find,"%s",g_find_open?g_find:"");
    p->font_px=g_font_px;p->cell_w=g_render.cell_w;p->cell_h=g_render.cell_h;
    p->link_len=(uint32_t)snprintf(p->link,sizeof p->link,"%s",g_link_set?g_link_url:"");
    p->origin_x=g_origin_x;p->origin_y=g_origin_y;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&p->serial,p->serial+1,__ATOMIC_RELEASE);
}

/* --- runtime hooks -------------------------------------------------------- */

static bool cursor_blinks(void);
static bool cursor_phase(uint64_t now);
static bool text_phase(uint64_t now);
/* Whether the window has the keyboard: the cursor goes hollow without it, and
 * the shown tab's program is told if it asked (DECSET 1004); a tab put away
 * has lost it, the one brought forward has it. */
static void term_focus(void) {
    static uint64_t next;
    uint64_t now=embk_uptime_ms();
    if (now>=next) {
        next=now+100;
        bool f=embk_win_focused()!=0;
        if (f!=g_focused) {g_focused=f;g_activity=now;em_request_frame();}
    }
    for (int i=0;i<g_tabs;i++) {
        struct session *s=g_tab[i];
        int want=g_focused && i==g_active;
        if (s->focus_reported==want || s->dead) continue;
        char out[4];size_t n=vt_focus_report(&s->vt,want!=0,out);
        if (n) (void)send(s,out,n);
        s->focus_reported=want;
    }
}
/* One session: the size the kernel knows, its output into the model, its
 * prompt, directory and shell. Returns whether the shown picture changed. */
static bool session_poll(struct session *s,bool shown) {
    bool got=false;
    if (!s->spawned && g_render.cols) {start_shell(s);got=true;}
    flush(s);
    if (s->master>=0 && (s->cols!=g_cols || s->rows!=g_rows)) {
        struct embk_terminal_size size={(uint16_t)g_rows,(uint16_t)g_cols,0,0};
        if (embk_terminal_control(s->master,EMBK_TERMINAL_SET_SIZE,&size,sizeof size)==0) {
            s->cols=g_cols;s->rows=g_rows;
            if (s->size_report && s->vt.inband_resize) {char r[48];size_t n=vt_size_report(&s->vt,r);(void)send(s,r,n);}
            s->size_report=false;
        }
    }
    size_t budget=32768;
    while (budget && s->master>=0) {
        int64_t n=embk_fd_avail(s->master);
        if (n<=0) break;
        char buf[1024];
        size_t want=n<(int64_t)sizeof buf?(size_t)n:sizeof buf;
        if (want>budget) want=budget;
        int r=(int)embk_read(s->master,buf,want);
        if (r<=0) break;
        uint64_t scrolled=s->vt.scrolls;
        vt_feed(&s->vt,buf,(size_t)r);
        /* reading the scrollback, the view stays on what it shows */
        if (s->view>0) {s->view+=(int)(s->vt.scrolls-scrolled);if (s->view>(int)s->vt.history_rows) s->view=(int)s->vt.history_rows;}
        got=true;budget-=(size_t)r;
        if (shown) g_activity=embk_uptime_ms();
    }
    if (s->prompt_serial!=s->vt.prompt_serial) {s->prompt_serial=s->vt.prompt_serial;s->at_prompt=s->vt.shell_ready;got=true;}
    if (strcmp(s->directory,s->vt.directory)) {
        memcpy(s->directory,s->vt.directory,sizeof s->directory);
        if (s->directory[0]) snprintf(s->cwd,sizeof s->cwd,"%.*s",(int)sizeof s->cwd-1,s->directory);
        got=true;
    }
    /* OSC 52, when allowed: the program's text becomes the clipboard */
    if (s->vt.clipboard_pending) {
        s->vt.clipboard_pending=false;
        if (embk_clip_set(s->vt.clipboard,s->vt.clipboard_len)>=0) snprintf(g_notice,sizeof g_notice,"Copied by the program");
        got=true;
    }
    if (s->sel.active && s->vt.alt!=s->sel_alt) term_select_clear(&s->sel);   /* made on the other screen */
    if (!s->dead && s->shell>=0 && !embk_proc_alive(s->shell)) {
        s->dead=true;embk_wait(s->shell);s->shell=-1;
        say(s,g_tabs>1?"\n[shell exited -- Escape closes this tab]\n":"\n[shell exited -- press ESC to close]\n");
        got=true;
    }
    return got;
}
static void term_idle(void) {
    bool got=false;
    term_focus();
    for (int i=0;i<g_tabs;i++) {
        char before[64];snprintf(before,sizeof before,"%s",i!=g_active?tab_title(g_tab[i]):"");
        bool changed=session_poll(g_tab[i],i==g_active);
        if (i==g_active) got|=changed;
        else if (changed && strcmp(before,tab_title(g_tab[i]))) em_request_frame();   /* its tab's title */
    }
    reap();
    /* blinking text and a blinking cursor: a frame when a phase turns */
    uint64_t now=embk_uptime_ms();
    static int phase=-1;
    int p=(g_render.blink_text && text_phase(now) ? 1 : 0)|(cursor_blinks() && cursor_phase(now) ? 2 : 0);
    if (p!=phase) {phase=p;em_request_frame();}
    if (got) {probe_refresh();em_request_frame();}
}

/* --- the clipboard ------------------------------------------------------------- */

static void clip(const char *text,size_t n,const char *done) {
    if (embk_clip_set(text,n)<0) snprintf(g_notice,sizeof g_notice,"The clipboard refused the text.");
    else snprintf(g_notice,sizeof g_notice,"%s",done);
    em_request_frame();
}
/* Copy: the selection; with nothing selected, the rows on the screen. */
static void term_copy(void) {
    static char copy[65536];size_t n=0;
    struct session *s=S;
    if (s->sel.active) {
        n=term_select_text(&s->sel,&s->vt,copy,sizeof copy);
        if (n) {clip(copy,n,"Copied");return;}
    }
    for (int y=0;y<s->vt.rows;y++) {
        static char row[VT_MAX_COLS*12+1];size_t k=vt_line_text(&s->vt,y,(unsigned)s->view,row,sizeof row);
        if (n+k+1>sizeof copy) {snprintf(g_notice,sizeof g_notice,"Screen text exceeds the clipboard limit.");em_request_frame();return;}
        memcpy(copy+n,row,k);n+=k;
        if (y+1<s->vt.rows && !vt_view_wrapped(&s->vt,y,(unsigned)s->view)) copy[n++]='\n';
    }
    clip(copy,n,"Screen copied");
}
static void term_paste(void) {
    struct session *s=S;
    if (s->dead) return;
    static char clipboard[65536],paste[65568];
    int64_t held=embk_clip_get(clipboard,sizeof clipboard);
    if (held<=0) return;
    if (held>(int64_t)sizeof clipboard) {snprintf(g_notice,sizeof g_notice,"Clipboard text is too large to paste.");em_request_frame();return;}
    /* Bracketed when the program asked (the shells do, at their prompts), so
     * the text arrives as text; otherwise a line break is Enter, as Terminal.app
     * pastes. Control bytes other than tab and newline are dropped. */
    bool bracketed=s->vt.bracketed_paste;
    size_t n=0;if (bracketed) {memcpy(paste,"\033[200~",6);n=6;}
    for (int64_t i=0;i<held;i++) {
        unsigned char c=(unsigned char)clipboard[i];
        if (c=='\r') {if (i+1<held && clipboard[i+1]=='\n') continue;c='\n';}
        if (c==127 || (c<32 && c!='\n' && c!='\t')) continue;
        if (!bracketed && c=='\n') c='\r';
        paste[n++]=(char)c;
    }
    if (bracketed) {memcpy(paste+n,"\033[201~",6);n+=6;}
    (void)send(s,paste,n);
    s->view=0;
    em_request_frame();
}
/* Clear: the scrollback and the screen. A shell at its prompt is asked to
 * redraw it (Ctrl+L) rather than having it wiped from under its editor. */
static void term_clear(void) {
    struct session *s=S;
    s->view=0;term_select_clear(&s->sel);
    if (s->at_prompt && s->vt.edit_shell && !s->dead) {vt_clear_history(&s->vt);(void)send(s,"\f",1);}
    else vt_feed(&s->vt,"\033[2J\033[H\033[3J",11);
    em_request_frame();
}

/* --- links ------------------------------------------------------------------------
 * A web link opens in Vellum, a file:// one in Files (a folder) or Editor (a
 * file); each gets what it reads at start (VELLUM_URL, FILES_PATH,
 * EDIT_FILE), as the desktop starts them. */
static int hexval(char c) {return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:-1;}
static void open_link(const char *url) {
    char env0[512],env1[200];
    const char *path=NULL;char *argv[2]={NULL,NULL};
    snprintf(env1,sizeof env1,"HOME=%s",home_dir());
    if (!strncmp(url,"file://",7)) {
        const char *p=strchr(url+7,'/');
        char file[256];size_t n=0;
        for (;p && *p && n+1<sizeof file;p++) {
            if (*p=='%' && hexval(p[1])>=0 && hexval(p[2])>=0) {file[n++]=(char)(hexval(p[1])*16+hexval(p[2]));p+=2;}
            else file[n++]=*p;
        }
        file[n]=0;
        struct embk_stat st;
        bool dir=embk_stat(file,&st)==0 && st.type==EMBK_DT_DIR;
        path=dir?"/data/apps/files/files.elf":"/data/apps/edit/edit.elf";
        snprintf(env0,sizeof env0,"%s=%s",dir?"FILES_PATH":"EDIT_FILE",file);
    } else if (!strncmp(url,"http://",7) || !strncmp(url,"https://",8)) {
        path="/data/apps/vellum/vellum.elf";
        snprintf(env0,sizeof env0,"VELLUM_URL=%s",url);
    } else {
        snprintf(g_notice,sizeof g_notice,"No app here opens %.*s links.",(int)(strcspn(url,":")),url);
        em_request_frame();return;
    }
    argv[0]=(char *)path;
    char *env[]={env0,env1,NULL};
    if (embk_spawn_env(path,argv,env,NULL,0)<0) snprintf(g_notice,sizeof g_notice,"Could not open the link.");
    else snprintf(g_notice,sizeof g_notice,"Opened %.70s",url);
    em_request_frame();
}

/* --- find ---------------------------------------------------------------------------
 * Super+F: a field over the screen. Typing finds the last match on the screen
 * or in the scrollback; Enter (and the up arrow button) the one before it,
 * Shift+Enter (the down button) the one after, going round at the ends. The
 * matches are marked; the view moves to the one found. */
static char g_find_seen[128];
static bool g_find_focus;
static void find_show(struct session *s,const struct term_match *m) {
    if (vt_row_in_view(&s->vt,m->start.row,(unsigned)s->view)>=0) return;
    int64_t back=vt_row_id(&s->vt,0,0)+s->vt.rows/2-m->start.row;
    if (back<0) back=0;
    if (back>(int64_t)s->vt.history_rows) back=s->vt.history_rows;
    s->view=(int)back;
}
static void find_step(bool older) {
    struct session *s=S;
    struct term_point end={vt_row_id(&s->vt,s->vt.rows-1,0),s->vt.cols+1};
    struct term_point top={vt_row_id(&s->vt,0,s->vt.alt?0:s->vt.history_rows),-1};
    struct term_point from=g_find_found?g_find_match.start:(older?end:top);
    struct term_match m;
    bool found=g_find[0] && term_find(&s->vt,g_find,from,older,&m);
    if (!found && g_find[0]) found=term_find(&s->vt,g_find,older?end:top,older,&m);   /* round from the other end */
    g_find_found=found;
    if (found) {g_find_match=m;find_show(s,&m);}
    probe_refresh();em_request_frame();
}
static void find_open(void) { g_find_open=true;g_find_focus=true;em_request_frame(); }
static void find_close(void) {
    g_find_open=false;g_find_found=false;
    ui_request_focus(INSTANCE_HANDLE_NULL);
    probe_refresh();em_request_frame();
}

/* --- zoom -------------------------------------------------------------------------- */

static int g_zoom;                    /* pixels added to the font, -6..+18 */
static bool g_fonts_stale=true;
static void zoom(int by) {
    int z=by?g_zoom+by:0;
    if (z<-6) z=-6;
    if (z>18) z=18;
    if (z==g_zoom && by) return;
    g_zoom=z;g_fonts_stale=true;em_request_frame();
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
static size_t encode_key(struct vt *v,int c,unsigned key,unsigned code,unsigned mods,char out[16]) {
    unsigned vtmods=vt_mods(mods);
    if (key) return vt_key_sequence(v,key,vtmods,out);
    if (c&EM_KEY_ONLY) return 0;
    if (!code) {out[0]=(char)c;return 1;}             /* a character's later UTF-8 byte */
    int byte=c;
    if ((mods&EMBK_KM_CTRL) && byte>=0x20 && byte<0x7f) {int ctl=control_byte(code);if (ctl>=0) byte=ctl;}
    return vt_key_sequence(v,(unsigned)byte,vtmods&2,out);
}
static void view_scroll(int rows) {
    struct session *s=S;
    if (s->vt.alt) return;
    s->view+=rows;
    if (s->view<0) s->view=0;
    if (s->view>(int)s->vt.history_rows) s->view=(int)s->vt.history_rows;
}
/* Super with a key: the window's own commands; never the program's. */
static void super_key(int c,unsigned code,unsigned mods) {
    switch (code) {
    case 't': tab_new();break;
    case 'f': find_open();break;
    case 'g': if (g_find_open) find_step(!(mods&EMBK_KM_SHIFT));break;
    case '=': case '+': zoom(1);break;
    case '-': zoom(-1);break;
    case '0': zoom(0);break;
    case '7': case '8': case '9': tab_show(code=='9'?g_tabs-1:(int)(code-'1'));break;   /* Super+1-6 are the spaces' */
    default: (void)c;break;
    }
    em_request_frame();
}

/* Keys are encoded for the shown tab's program. Shift+PgUp/PgDn/Home/End and
 * the wheel move through the scrollback; Super shortcuts never reach a
 * program; Ctrl+Tab and Ctrl+Shift+Tab go between tabs; once a tab's shell
 * has exited, Escape closes the tab. While the find field has the keyboard,
 * keys are the field's. */
static int term_key(int c) {
    unsigned mods=(unsigned)em_key_modifiers(),code=(unsigned)em_key_code(),flags=(unsigned)em_key_flags();g_notice[0]=0;
    g_activity=embk_uptime_ms();
    if (!g_tabs) return 1;
    struct session *s=S;
    unsigned key=key_identity(c,code,flags);
    if (g_find_open && ui_any_focus()) {
        if (key==VT_KEY_ESCAPE) {find_close();return 1;}
        if ((mods&EMBK_KM_GUI) && code=='g') {find_step(!(mods&EMBK_KM_SHIFT));return 1;}
        if (key==VT_KEY_ENTER && (mods&EMBK_KM_SHIFT)) {find_step(false);return 1;}
        return 0;
    }
    if (c==EMBK_KEY_COPY || (c==3 && (mods&(EMBK_KM_CTRL|EMBK_KM_SHIFT))==(EMBK_KM_CTRL|EMBK_KM_SHIFT))) {term_copy();return 1;}
    if (c==EMBK_KEY_PASTE || (c==22 && (mods&EMBK_KM_SHIFT))) {term_paste();return 1;}
    if (c==EMBK_KEY_SEL_ALL) {term_select_all(&s->sel,&s->vt);s->sel_alt=s->vt.alt;probe_refresh();em_request_frame();return 1;}
    if (key==VT_KEY_TAB && (mods&EMBK_KM_CTRL)) {
        if (g_tabs>1) tab_show((g_active+((mods&EMBK_KM_SHIFT)?g_tabs-1:1))%g_tabs);
        return 1;
    }
    if (mods&EMBK_KM_SHIFT) {
        int page=s->vt.rows/2;
        switch (key) {
        case VT_KEY_PAGEUP: view_scroll(page);em_request_frame();return 1;
        case VT_KEY_PAGEDOWN: view_scroll(-page);em_request_frame();return 1;
        case VT_KEY_HOME: view_scroll((int)s->vt.history_rows);em_request_frame();return 1;
        case VT_KEY_END: view_scroll(-(int)s->vt.history_rows);em_request_frame();return 1;
        }
    }
    if (mods&EMBK_KM_GUI) {super_key(c,code,mods);return 1;}
    if (g_find_open && key==VT_KEY_ESCAPE) {find_close();return 1;}
    if (s->dead) {
        if (key==VT_KEY_ESCAPE) {if (g_tabs>1) {tab_close(g_active);return 1;} return 0;}   /* 0: the runtime closes the window */
        return 1;
    }
    char sequence[16];size_t n=encode_key(&s->vt,c,key,code,mods,sequence);
    if (n) (void)send(s,sequence,n);
    if (s->view) s->view=0;
    em_request_frame();
    return 1;                                    /* never reaches the toolkit */
}

/* --- the mouse -------------------------------------------------------------
 * What the program asked for (DECSET 9/1000/1002/1003) is reported in the
 * encoding it chose; Shift keeps the mouse for the terminal. Otherwise the
 * left button selects -- a drag characters, a double click a word, a triple
 * click a line, Alt a rectangle, Shift-click extends -- and Super with a click
 * opens a link. The wheel moves through the scrollback, or in the alternate
 * screen (less, man, vim) sends Up and Down, as Terminal.app does. The toolkit
 * hands over the pointer once a frame: presses are counted by the toolkit
 * (several can come between two frames), drags and releases are the button
 * state's changes. */
static unsigned g_buttons;
static int g_mouse_col=-1,g_mouse_row=-1;
static bool g_selecting;
static bool g_menu_open;
static float g_menu_x,g_menu_y;
static char g_menu_link[256];
static void report(struct session *s,enum vt_mouse_event event,int button,int col,int row,int px,int py,unsigned mods) {
    char out[48];size_t n=vt_mouse_report(&s->vt,event,button,col,row,px,py,mods,out);
    if (n) (void)send(s,out,n);
}
/* The point a selection runs to: a cell for words and lines, the edge
 * between two cells for characters and rectangles. */
static struct term_point select_point(const struct session *s,enum term_select_mode mode,int px,int row) {
    int cw=g_render.cell_w;
    int col=mode==TERM_SELECT_WORDS || mode==TERM_SELECT_LINES?px/cw:(px+cw/2)/cw;
    if (col<0) col=0;
    if (col>s->vt.cols) col=s->vt.cols;
    if (row<0) row=0;
    if (row>=s->vt.rows) row=s->vt.rows-1;
    struct term_point p={vt_row_id(&s->vt,row,(unsigned)s->view),col};
    return p;
}
static void term_mouse(float ox,float oy,float wheel) {
    struct session *s=S;
    float mx,my;ui_pointer_pos(&mx,&my);
    int cw=g_render.cell_w,ch=g_render.cell_h;
    if (!cw || !ch) return;
    int px=(int)floorf(mx-ox),py=(int)floorf(my-oy);
    int col=px<0?-1:px/cw,row=py<0?-1:py/ch;
    bool inside=col>=0 && row>=0 && col<s->vt.cols && row<s->vt.rows;
    if (col<0) col=0;
    if (row<0) row=0;
    if (col>=s->vt.cols) col=s->vt.cols-1;
    if (row>=s->vt.rows) row=s->vt.rows-1;
    unsigned buttons=(unsigned)em_pointer_buttons(),keys=(unsigned)embk_key_mods();
    unsigned mods=vt_mods(keys);
    uint64_t when;int clicks=em_take_clicks(&when);
    int notches=(int)lroundf(wheel);
    struct term_point cell={vt_row_id(&s->vt,row,(unsigned)s->view),col};
    /* the link under the pointer, while Super is held */
    bool was_link=g_link_set;
    g_link_set=inside && (keys&EMBK_KM_GUI) && term_link_at(&s->vt,cell,g_link_url,sizeof g_link_url,&g_link);
    if (g_link_set!=was_link) {probe_refresh();em_request_frame();}
    if (s->vt.mouse!=VT_MOUSE_OFF && !(keys&EMBK_KM_SHIFT) && !s->dead) {
        static const unsigned bit[3]={EMBK_MOUSE_LEFT,EMBK_MOUSE_MIDDLE,EMBK_MOUSE_RIGHT};
        for (int b=0;b<3;b++) {
            bool now=(buttons&bit[b])!=0,was=(g_buttons&bit[b])!=0;
            if (now && !was && inside) report(s,VT_MOUSE_PRESS,b,col,row,px,py,mods);
            else if (!now && was) report(s,VT_MOUSE_RELEASE,b,col,row,px,py,mods);
        }
        if ((col!=g_mouse_col || row!=g_mouse_row) && (inside || buttons)) {
            int held=buttons&EMBK_MOUSE_LEFT?0:buttons&EMBK_MOUSE_MIDDLE?1:buttons&EMBK_MOUSE_RIGHT?2:3;
            report(s,VT_MOUSE_MOTION,held,col,row,px,py,mods);
        }
        for (int k=notches>0?notches:-notches;k-- && inside;) report(s,VT_MOUSE_PRESS,notches>0?4:5,col,row,px,py,mods);
        float rx,ry;if (em_right_peek(&rx,&ry)) em_right_take();    /* the program's, not the menu's */
        g_selecting=false;
    } else {
        if (clicks && inside) {
            ui_request_focus(INSTANCE_HANDLE_NULL);              /* a click on the screen takes the keyboard back */
            int run=1;
            for (int i=0;i<clicks;i++) run=ui_click_note(mx,my);
            if ((keys&EMBK_KM_GUI) && g_link_set) open_link(g_link_url);
            else if ((keys&EMBK_KM_SHIFT) && s->sel.active && run==1) {
                term_select_extend(&s->sel,&s->vt,select_point(s,s->sel.mode,px,row));
                g_selecting=(buttons&EMBK_MOUSE_LEFT)!=0;
            } else {
                enum term_select_mode mode=run>=3?TERM_SELECT_LINES:run==2?TERM_SELECT_WORDS:
                                          (keys&EMBK_KM_ALT)?TERM_SELECT_BLOCK:TERM_SELECT_CHARS;
                term_select_start(&s->sel,&s->vt,select_point(s,mode,px,row),mode);
                s->sel_alt=s->vt.alt;
                g_selecting=true;
            }
            probe_refresh();em_request_frame();
        }
        if (g_selecting) {
            if (buttons&EMBK_MOUSE_LEFT) {
                /* past the top or the bottom, the view follows */
                if (py<0) view_scroll(1);
                else if (py>=s->vt.rows*ch) view_scroll(-1);
                if (py<0 || py>=s->vt.rows*ch) em_request_frame();
                struct term_point p=select_point(s,s->sel.mode,px,py<0?0:py/ch);
                if (p.row!=s->sel.head.row || p.col!=s->sel.head.col) {term_select_extend(&s->sel,&s->vt,p);em_request_frame();}
            } else {
                g_selecting=false;
                /* a click that did not move selects nothing */
                if ((s->sel.mode==TERM_SELECT_CHARS || s->sel.mode==TERM_SELECT_BLOCK) &&
                    s->sel.anchor.row==s->sel.head.row && s->sel.anchor.col==s->sel.head.col) term_select_clear(&s->sel);
                probe_refresh();em_request_frame();
            }
        }
        if (notches && s->vt.alt && !s->dead) {
            char seq[16];size_t n=vt_key_sequence(&s->vt,notches>0?VT_KEY_UP:VT_KEY_DOWN,0,seq);
            for (int k=(notches>0?notches:-notches)*3;k--;) (void)send(s,seq,n);
        } else if (notches) view_scroll(notches*3);
        float rx,ry;
        if (RightClicked(&rx,&ry)) {
            g_menu_open=true;g_menu_x=rx;g_menu_y=ry;
            struct term_match where;
            if (!(inside && term_link_at(&s->vt,cell,g_menu_link,sizeof g_menu_link,&where))) g_menu_link[0]=0;
        }
    }
    g_buttons=buttons;g_mouse_col=col;g_mouse_row=row;
}

/* --- the view -------------------------------------------------------------- */

static bool g_bell_drawn;
static uint64_t g_sync_since,g_bell_until;
static unsigned g_bells_seen;
static int g_dmg_y0,g_dmg_y1;
static float g_screen_top;            /* where the screen began last frame, in the window */
#define CURSOR_BLINK_MS 530
#define TEXT_BLINK_MS   500
#define TAB_ON  (ui_theme()->surface_alt)
#define TAB_OFF ((Color){0,0,0,0})

static uint32_t rgb(Color c) {
    return ((uint32_t)(c.r*255+.5f)<<16)|((uint32_t)(c.g*255+.5f)<<8)|(uint32_t)(c.b*255+.5f);
}
static uint32_t blend_rgb(uint32_t a,uint32_t b,unsigned w) {   /* w/256 of a */
    uint32_t out=0;
    for (int sh=0;sh<24;sh+=8) out|=((((a>>sh)&255)*w+((b>>sh)&255)*(256-w))>>8)<<sh;
    return out;
}
/* The faces: MesloLGS Nerd Font Mono, as the Mac's profile -- 14 px at the
 * standard interface size, following the interface's scale, and the zoom --
 * and DejaVu after it for what Meslo lacks. */
static bool load_fonts(const struct ui_theme *t) {
    static float loaded;
    float size=14.0f*t->text_caption/12.0f+(float)g_zoom;
    if (!g_fonts_stale && size==loaded) return true;
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
    loaded=size;g_font_px=size;g_fonts_stale=false;
    if (g_zoom) snprintf(g_notice,sizeof g_notice,"%.0f px",size);
    return true;
}
/* The profile's colours: until Preferences exist (docs/TODO.md), the theme's
 * text and background with the built-in 256-colour palette. Applied to every
 * tab's model, so text already on the screen follows a theme change. */
static uint32_t g_palette[3];
static void apply_palette_to(struct vt *v) {
    struct vt_palette p;
    for (unsigned i=0;i<256;i++) p.color[i]=vt_default_color(i);
    p.fg=g_palette[0];p.bg=g_palette[1];p.cursor=g_palette[2];
    vt_set_palette(v,&p);
}
static void apply_palette(const struct ui_theme *t) {
    uint32_t want[3]={rgb(t->text),rgb(t->bg),rgb(t->accent)};
    if (!memcmp(g_palette,want,sizeof want)) return;
    memcpy(g_palette,want,sizeof want);
    for (int i=0;i<g_tabs;i++) apply_palette_to(&g_tab[i]->vt);
}
/* DECSCUSR 1, 3, 5 blink; 0 (the default block) only with DECSET 12. */
static bool cursor_blinks(void) {
    if (!g_tabs) return false;
    int c=S->vt.cursor_style;
    return c==1 || c==3 || c==5 || (c==0 && S->vt.cursor_blink);
}
static bool cursor_phase(uint64_t now) {
    return !cursor_blinks() || !g_focused || ((now-g_activity)/CURSOR_BLINK_MS)%2==0;
}
static bool text_phase(uint64_t now) { return (now/TEXT_BLINK_MS)%2==0; }
static void bell_frame(void) {
    uint32_t a=vt_cursor_rgb(&S->vt);int w=g_render.width,h=g_render.height;
    for (int y=0;y<h;y++) {
        uint32_t *row=g_render.pixels+(size_t)y*w;
        for (int x=0;x<w;x++) if (y<2 || y>=h-2 || x<2 || x>=w-2) row[x]=0xff000000u|a;
    }
}
/* Every tab takes the window's size: its model reflows (the shell's prompt
 * erased first, to be drawn again on the in-band report). */
static bool resize_all(int cols,int rows) {
    for (int i=0;i<g_tabs;i++) {
        struct session *s=g_tab[i];
        bool erased=s->vt.inband_resize && vt_prompt_erase(&s->vt);
        if (s->vt.inband_resize) s->size_report=true;
        if (!vt_resize(&s->vt,cols,rows)) return false;
        if (erased) vt_prompt_here(&s->vt);
        term_select_clear(&s->sel);         /* its rows are new rows now */
    }
    if (!term_render_resize(&g_render,cols,rows)) return false;
    for (int i=0;i<g_tabs;i++) {g_tab[i]->vt.pixel_width=g_render.width;g_tab[i]->vt.pixel_height=g_render.height;}
    g_find_found=false;
    return true;
}
static bool screen_bitmap(void) {
    const struct ui_theme *t=ui_theme();
    if (!g_tabs || !load_fonts(t)) return false;
    struct session *s=S;
    int cw=g_render.cell_w,ch=g_render.cell_h;
    float top=g_screen_top>0?g_screen_top:(float)t->toolbar_height+10;
    int cols=(int)((em_viewport_width()-28)/cw),rows=(int)((em_viewport_height()-top-10)/ch);
    if (cols<2) cols=2;
    if (cols>VT_MAX_COLS) cols=VT_MAX_COLS;
    if (rows<2) rows=2;
    if (rows>VT_MAX_ROWS) rows=VT_MAX_ROWS;
    if (cols!=g_render.cols || rows!=g_render.rows || !g_render.pixels) {
        if (!resize_all(cols,rows)) return false;
    }
    g_cols=cols;g_rows=rows;
    if (s->view>(int)s->vt.history_rows) s->view=(int)s->vt.history_rows;
    if (s->vt.alt) s->view=0;
    apply_palette(t);
    uint64_t now=embk_uptime_ms();
    g_dmg_y0=g_dmg_y1=0;
    /* DECSET 2026: a program drawing a frame in parts is shown the whole
     * frame -- held for at most 200 ms, so one that never ends it still shows */
    if (s->vt.sync_output) {
        if (!g_sync_since) g_sync_since=now?now:1;
        if (g_frames && now-g_sync_since<200) {em_request_frame();return true;}
    } else g_sync_since=0;
    if (s->vt.bells!=g_bells_seen) {g_bells_seen=s->vt.bells;g_bell_until=now+150;}
    bool bell=now<g_bell_until;
    if (bell) em_request_frame();
    if (bell!=g_bell_drawn) term_render_invalidate(&g_render);   /* the visual bell comes or goes */
    struct vt_cursor *cursor=&vt_active(&s->vt)->cursor;
    uint32_t bg=g_palette[1],accent=g_palette[2];
    struct term_frame fr={
        .back=(unsigned)s->view,
        .cursor=!s->view && !s->dead && s->vt.cursor_visible && cursor_phase(now),
        .cursor_x=cursor->x,.cursor_y=cursor->y,.cursor_shape=s->vt.cursor_style,
        .cursor_hollow=!g_focused,.blink_on=text_phase(now),.bold_is_bright=false,
        .selection=&s->sel,.find=g_find_open?g_find:NULL,
        .find_current_set=g_find_open && g_find_found,.find_current=g_find_match,
        .link_set=g_link_set,.link=g_link,
        .selection_bg=blend_rgb(accent,bg,100),.find_bg=0xc9a227,.find_current_bg=0xff8c1a,.find_fg=0x111111,
    };
    bool painted=term_render_frame(&g_render,&s->vt,&fr);
    if (bell) {bell_frame();g_dmg_y0=0;g_dmg_y1=g_render.height;}
    else if (painted) {g_dmg_y0=g_render.dmg_y0;g_dmg_y1=g_render.dmg_y1;}
    g_bell_drawn=bell;
    if (painted || bell) g_frames++;
    /* The probe follows what is shown, not the blink: a reader takes longer
     * than a blink phase, and a probe rewritten under it is never whole. */
    static uint64_t probed_revision;static int probed_view=-1,probed_tab=-1;
    if (painted && (s->vt.revision!=probed_revision || s->view!=probed_view || g_active!=probed_tab)) {
        probed_revision=s->vt.revision;probed_view=s->view;probed_tab=g_active;probe_refresh();
    }
    return true;
}

/* Clicks on the tabs and the menu are acted on at the start of the next
 * frame: a frame that has drawn a tab's button must not free the tab. */
static int g_switch_to=-1,g_close_tab=-1;
static bool g_want_tab,g_want_find,g_want_copy,g_want_paste,g_want_all,g_want_clear;
static char g_want_link[256],g_want_copy_link[256];
static void deferred(void) {
    if (g_close_tab>=0) {tab_close(g_close_tab);g_close_tab=-1;}
    if (!g_tabs) return;
    if (g_switch_to>=0) {tab_show(g_switch_to);g_switch_to=-1;}
    if (g_want_tab) {g_want_tab=false;tab_new();}
    if (g_want_find) {g_want_find=false;find_open();}
    if (g_want_copy) {g_want_copy=false;term_copy();}
    if (g_want_paste) {g_want_paste=false;term_paste();}
    if (g_want_all) {g_want_all=false;term_select_all(&S->sel,&S->vt);S->sel_alt=S->vt.alt;probe_refresh();}
    if (g_want_clear) {g_want_clear=false;term_clear();}
    if (g_want_link[0]) {open_link(g_want_link);g_want_link[0]=0;}
    if (g_want_copy_link[0]) {clip(g_want_copy_link,strlen(g_want_copy_link),"Link copied");g_want_copy_link[0]=0;}
}
static void tab_strip(void) {
    static const char *const KEY[TABS_MAX]={"t0","t1","t2","t3","t4","t5","t6","t7","t8","t9","t10","t11","t12","t13","t14","t15"};
    static const char *const KEYL[TABS_MAX]={"l0","l1","l2","l3","l4","l5","l6","l7","l8","l9","l10","l11","l12","l13","l14","l15"};
    static const char *const KEYX[TABS_MAX]={"x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15"};
    const struct ui_theme *t=ui_theme();
    HStack(.spacing=3,.align=Center,.px=10,.py=3,.key="tabs") {
        for (int i=0;i<g_tabs;i++) {
            bool cur=i==g_active;
            HStack(.spacing=0,.align=Center,.px=3,.corner=8,.background=cur?TAB_ON:TAB_OFF,.key=KEY[i]) {
                EmV tb=Button(tab_title(g_tab[i])).ghost().font(Caption).py(3).px(8).id(KEYL[i]);
                tb.color(cur?t->text:t->text_secondary);
                if (tb.clicked() && !cur) g_switch_to=i;
                if (Button("\xc3\x97").ghost().font(Caption).py(3).px(5).color(t->text_secondary).id(KEYX[i]).clicked())
                    g_close_tab=i;
            }
        }
        if (Button("+").ghost().font(Caption).py(3).px(8).id("tnew").clicked()) g_want_tab=true;
        Spacer();
    }
    Divider("tabsep");
}
static void find_bar(void) {
    const struct ui_theme *t=ui_theme();
    HStack(.spacing=6,.align=Center,.px=12,.py=4,.key="findbar") {
        if (g_find_focus) {em_field_autofocus();g_find_focus=false;}
        if (TextField(g_find,sizeof g_find,"Find in the scrollback").submitted()) find_step(true);
        if (strcmp(g_find,g_find_seen)) {                  /* typed: from the bottom again */
            snprintf(g_find_seen,sizeof g_find_seen,"%s",g_find);
            g_find_found=false;
            if (g_find[0]) find_step(true); else {probe_refresh();em_request_frame();}
        }
        Text(!g_find[0]?"":g_find_found?"":"Not found").caption().color(t->text_secondary);
        if (Button("\xe2\x96\xb2").ghost().font(Caption).py(2).id("fup").clicked()) find_step(true);
        if (Button("\xe2\x96\xbc").ghost().font(Caption).py(2).id("fdown").clicked()) find_step(false);
        if (Button("Done").ghost().font(Caption).py(2).id("fdone").clicked()) find_close();
    }
    Divider("findsep");
}
static void context_menu(void) {
    ContextMenu(&g_menu_open,g_menu_x,g_menu_y) {
        if (g_menu_link[0]) {
            if (MenuItem("Open Link")) snprintf(g_want_link,sizeof g_want_link,"%s",g_menu_link);
            if (MenuItem("Copy Link")) snprintf(g_want_copy_link,sizeof g_want_copy_link,"%s",g_menu_link);
            MenuSeparator();
        }
        if (MenuItemK("Copy","Super+C")) g_want_copy=true;
        if (MenuItemK("Paste","Super+V")) g_want_paste=true;
        if (MenuItemK("Select All","Super+A")) g_want_all=true;
        MenuSeparator();
        if (MenuItemK("Find\xe2\x80\xa6","Super+F")) g_want_find=true;
        if (MenuItem("Clear Scrollback")) g_want_clear=true;
        MenuSeparator();
        if (MenuItemK("New Tab","Super+T")) g_want_tab=true;
        if (MenuItemK("Close Tab","Super+W")) g_close_tab=g_active;
    }
}
static void term_view(void) {
    deferred();
    const Color bg=ui_theme()->bg;
    Window("Terminal",.background=bg) {
        if (!g_tabs) {Spacer();return;}
        const char *title=g_notice[0]?g_notice:g_link_set?g_link_url:window_title(S);
        AppBar(title,.background=bg) {
            if (Button("+").ghost().id("bnew").clicked()) g_want_tab=true;
            if (Button("Copy").ghost().id("bcopy").clicked()) g_want_copy=true;
            if (Button("Paste").ghost().id("bpaste").clicked()) g_want_paste=true;
            if (Button("Clear").ghost().id("bclear").clicked()) g_want_clear=true;
        }
        if (g_tabs>1) tab_strip();
        if (g_find_open) find_bar();
        VStack(.spacing=0,.px=14,.pt=10,.pb=10,.align=Leading,.clip=1,.background=bg) {
            float wheel=ui_take_wheel();
            float bx=0,by=0,bw=0,bh=0;
            if (ui_open_rect(&bx,&by,&bw,&bh)) {
                if (by+10!=g_screen_top) {g_screen_top=by+10;em_request_frame();}   /* the bars above it moved it */
                if ((int)(bx+14)!=g_origin_x || (int)(by+10)!=g_origin_y) {g_origin_x=(int)(bx+14);g_origin_y=(int)(by+10);probe_refresh();}
                term_mouse(bx+14,by+10,wheel);    /* the image sits inside the padding */
            }
            if (screen_bitmap()) {
                em_flush();
                int w=g_render.width,h=g_render.height;
                ui_image_sized_damage(0x5445524d,g_render.pixels,(uint32_t)w,(uint32_t)h,(float)w,(float)h,
                                      0,g_dmg_y0,w,g_dmg_y1-g_dmg_y0);
            } else Text("Terminal display could not be resized.").secondary();
        }
        Spacer();
        context_menu();
    }
}

/* Super+W asks the window to close: with several tabs it is the shown tab
 * that closes. The window's close light and Super+Q close them all. */
static int term_close(void) {
    if (g_tabs>1 && em_close_reason()==EM_CLOSE_REQUEST) {tab_close(g_active);return 0;}
    return 1;
}

int main(void) {
    struct session *first=session_new(home_dir());
    if (!first) return 1;
    g_tab[g_tabs++]=first;
    probe_refresh();
    em_set_key_hook(term_key);
    em_key_hook_wants_key_only(1);              /* F1-F12, Insert, Menu */
    em_set_idle_hook(term_idle);
    em_set_close_hook(term_close);

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

    /* Close the masters before collecting the shells. A blocked slave read
     * sees EOF; no global console mode is involved. */
    for (int i=0;i<g_tabs;i++) {
        struct session *s=g_tab[i];
        if (s->master>=0) close(s->master);   /* an fd, so close() -- NOT a handle close */
        if (s->shell>=0) embk_wait(s->shell); /* ...so this returns */
        vt_free(&s->vt);free(s);
    }
    for (int i=0;i<g_nreap;i++) embk_wait(g_reap[i]);
    term_render_free(&g_render);
    return rc;
}
