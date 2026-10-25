/* EmbLink Terminal: a private PTY session with a native prompt editor.
 *
 * At the shell prompt we retain local history, completion and cursor editing.
 * Once a command starts, input goes directly to the PTY master. Programs choose
 * canonical/echo or raw mode through termios; their escape-key input uses ANSI
 * sequences. Output uses a bounded VT screen with primary/alternate grids and
 * streaming Unicode and cursor editing. TERM=emlink advertises this subset.
 * The master is nonblocking and input is queued so a slow child cannot freeze
 * the window. Closing the master hangs up the slave, including blocked reads.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "../../terminal/vt.h"
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
/* The terminal edits the line itself only for a shell that does not (an A
 * mark without edit=shell -- the sh port, until it has the editor too). */
static bool g_local=false;
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

/* --- the command line ------------------------------------------------------
 * The terminal owns the line: nothing reaches the shell until Enter. g_cur is
 * the caret -- edits happen AT it, not only at the end. */
#define IN_MAX 256
#define HIST_MAX 32
static char g_in[IN_MAX];
static int  g_in_n = 0;
static int  g_cur = 0;         /* caret: 0..g_in_n */
static char g_hist[HIST_MAX][IN_MAX];
static int  g_hist_n = 0;      /* how many commands remembered */
static int  g_hist_at = -1;    /* -1 = editing a fresh line, else index back */

static void hist_push_line(const char *s) {
    if (!s[0]) return;
    if (g_hist_n && !strcmp(g_hist[(g_hist_n - 1) % HIST_MAX], s)) return;  /* no dupes */
    snprintf(g_hist[g_hist_n % HIST_MAX], IN_MAX, "%s", s);
    g_hist_n++;
}

/* Walk history: dir -1 = older (Up), +1 = newer (Down). */
static void hist_step(int dir) {
    int avail = g_hist_n < HIST_MAX ? g_hist_n : HIST_MAX;
    if (!avail) return;
    int at = g_hist_at + (dir < 0 ? 1 : -1);
    if (at < 0) {                       /* back to the fresh line */
        g_hist_at = -1; g_in[0] = 0; g_in_n = 0; g_cur = 0; return;
    }
    if (at >= avail) at = avail - 1;
    g_hist_at = at;
    const char *s = g_hist[(g_hist_n - 1 - at) % HIST_MAX];
    snprintf(g_in, sizeof g_in, "%s", s);
    g_in_n = (int)strlen(g_in);
    g_cur = g_in_n;
}

/* OSC 133 shell integration publishes a completed native prompt. Other
 * programs own their terminal screen and receive input directly. */
static char g_cwd[192]="/";

#define HOSTNAME "emblink"

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

static const char *prompt_text(void) {
    static char p[288];
    const char *user = getenv("USER");
    const char *home = getenv("HOME");
    if (!user || !*user) user = "user";
    char tilde[192];
    const char *shown = g_cwd;
    size_t hl = home ? strlen(home) : 0;
    /* ~ for home, as every shell does -- the full path is noise in your own
     * directory, which is where you spend most of your time. */
    if (hl && strncmp(g_cwd, home, hl) == 0 && (g_cwd[hl] == '/' || g_cwd[hl] == 0)) {
        snprintf(tilde, sizeof tilde, "~%s", g_cwd + hl);
        shown = tilde;
    }
    snprintf(p, sizeof p, "%s@%s:%s$", user, HOSTNAME, shown);
    return p;
}

static void term_clear(void) {
    vt_feed(&g_vt,"\033[2J\033[H\033[3J",11);g_view=0;
}
static void term_putc(char c) {
    vt_feed(&g_vt,&c,1);
}
static void term_newline(void) {vt_feed(&g_vt,"\r\n",2);}
static void term_say(const char *s) {
    while (*s) {if (*s=='\n')term_newline();else term_putc(*s);s++;}
}

/* --- line-editor edits ----------------------------------------------------- */

static void in_insert(char c) {
    if (g_in_n >= IN_MAX - 1) return;
    memmove(g_in + g_cur + 1, g_in + g_cur, (size_t)(g_in_n - g_cur) + 1);
    g_in[g_cur++] = c;
    g_in_n++;
}

static void in_backspace(void) {
    if (g_cur == 0) return;
    int start=g_cur-1;while(start>0 && ((unsigned char)g_in[start]&0xc0)==0x80)start--;
    memmove(g_in+start,g_in+g_cur,(size_t)(g_in_n-g_cur)+1);g_in_n-=g_cur-start;g_cur=start;
}

static void in_delete(void) {                    /* forward delete, at the caret */
    if (g_cur >= g_in_n) return;
    int end=g_cur+1;while(end<g_in_n && ((unsigned char)g_in[end]&0xc0)==0x80)end++;
    memmove(g_in+g_cur,g_in+end,(size_t)(g_in_n-end)+1);g_in_n-=end-g_cur;
}

static void in_kill_to_start(void) {             /* Ctrl+U */
    memmove(g_in, g_in + g_cur, (size_t)(g_in_n - g_cur) + 1);
    g_in_n -= g_cur; g_cur = 0;
}

static void in_kill_to_end(void) {               /* Ctrl+K */
    g_in[g_cur] = 0; g_in_n = g_cur;
}

static void in_kill_word(void) {                 /* Ctrl+W */
    int e = g_cur;
    while (g_cur > 0 && g_in[g_cur - 1] == ' ') g_cur--;
    while (g_cur > 0 && g_in[g_cur - 1] != ' ') g_cur--;
    memmove(g_in + g_cur, g_in + e, (size_t)(g_in_n - e) + 1);
    g_in_n -= e - g_cur;
}

/* --- Tab completion --------------------------------------------------------
 * The terminal owns the line, so it owns completion too. It completes the
 * token under the caret against the filesystem (relative to the cwd the shell
 * reports), and -- in command position -- against the shell's builtins and the
 * extern search directories. One match completes; several extend to the common
 * prefix and, when stuck, list into the transcript, exactly as bash does. */
static const char *k_builtins[] = {
    "ls","cat","cd","pwd","mkdir","rmdir","touch","rm","cp","mv","save",
    "ps","kill","env","uptime","date","clear","echo","where","select",
    "sort-by","first","head","last","tail","reverse","get","count","wc",
    "history","which","whoami","hostname","help","exit",
};

struct cand { char name[64]; int is_dir; };
#define CAND_MAX 96

static int cand_add(struct cand *cs, int n, const char *name, int is_dir) {
    if (n >= CAND_MAX) return n;
    snprintf(cs[n].name, sizeof cs[n].name, "%s", name);
    cs[n].is_dir = is_dir;
    return n + 1;
}

static void complete(void) {
    if (g_cur != g_in_n) return;               /* complete at line end only */

    /* the token being completed, and whether it names a command */
    int tok = g_in_n;
    while (tok > 0 && g_in[tok - 1] != ' ') tok--;
    int is_cmd = 1;
    for (int i = 0; i < tok; i++) if (g_in[i] != ' ') { is_cmd = 0; break; }
    const char *token = g_in + tok;

    /* split off any directory part; resolve it against the live cwd */
    const char *slash = strrchr(token, '/');
    char dir[224]; const char *base = token;
    if (slash) {
        size_t dl = (size_t)(slash - token);
        char dpart[160];
        if (dl >= sizeof dpart) return;
        memcpy(dpart, token, dl); dpart[dl] = 0;
        base = slash + 1;
        if (token[0] == '/')      snprintf(dir, sizeof dir, "%s", dl ? dpart : "/");
        else if (token[0] == '~') {
            const char *h = getenv("HOME");
            snprintf(dir, sizeof dir, "%s%s", h ? h : "", dpart + 1);
        }
        else snprintf(dir, sizeof dir, "%s/%s", g_cwd, dpart);
    } else {
        snprintf(dir, sizeof dir, "%s", g_cwd);
    }
    size_t blen = strlen(base);

    static struct cand cs[CAND_MAX];
    int n = 0;

    /* command position: builtins + the extern search dirs, bare names */
    if (is_cmd && !slash) {
        for (size_t i = 0; i < sizeof k_builtins / sizeof k_builtins[0]; i++)
            if (!strncmp(k_builtins[i], base, blen))
                n = cand_add(cs, n, k_builtins[i], 0);
        struct embk_dirent es[64];
        int64_t m = embk_readdir("/system/bin", es, 64);
        for (int64_t i = 0; i < m; i++) {
            size_t l = strlen(es[i].name);
            if (l > 4 && !strcmp(es[i].name + l - 4, ".elf")) {
                char bare[64];
                snprintf(bare, sizeof bare, "%.*s", (int)(l - 4), es[i].name);
                if (!strncmp(bare, base, blen)) n = cand_add(cs, n, bare, 0);
            }
        }
        m = embk_readdir("/data/apps", es, 64);
        for (int64_t i = 0; i < m; i++)
            if (es[i].type == EMBK_DT_DIR && es[i].name[0] != '.' &&
                !strncmp(es[i].name, base, blen))
                n = cand_add(cs, n, es[i].name, 0);
    }

    /* filesystem: everything in `dir` matching the base prefix */
    {
        struct embk_dirent es[64];
        int64_t m = embk_readdir(dir, es, 64);
        for (int64_t i = 0; i < m; i++) {
            if (es[i].name[0] == '.' && blen == 0) { /* hide dotfiles unprompted */ }
            else if (!strncmp(es[i].name, base, blen))
                n = cand_add(cs, n, es[i].name, es[i].type == EMBK_DT_DIR);
        }
    }
    if (n == 0) return;

    /* longest common prefix across every candidate */
    size_t common = strlen(cs[0].name);
    for (int i = 1; i < n; i++) {
        size_t j = 0;
        while (j < common && cs[i].name[j] == cs[0].name[j]) j++;
        common = j;
    }

    if (common > blen) {                       /* progress: extend the token */
        for (size_t j = blen; j < common && g_in_n < IN_MAX - 1; j++)
            in_insert(cs[0].name[j]);
        if (n == 1) in_insert(cs[0].is_dir ? '/' : ' ');
        return;
    }
    if (n == 1) { in_insert(cs[0].is_dir ? '/' : ' '); return; }

    /* stuck with several: show them, the way a second Tab does in bash */
    term_say(prompt_text()); term_putc(' '); term_say(g_in); term_newline();
    for (int i = 0; i < n; i++) {
        if (cs[i].is_dir) { term_say("\x1b[1;34m"); term_say(cs[i].name);
                            term_say("\x1b[0m/"); }
        else term_say(cs[i].name);
        term_say("  ");
    }
    term_newline();
}

/* --- the test probe ----------------------------------------------------------
 * What the window shows, kept current for the acceptance tools (term_probe.h).
 * Not static: tools find it by symbol, and a static that is only ever written
 * is a store the compiler may drop. */
__attribute__((used)) struct term_probe g_term_probe;
static uint32_t g_frames;

static size_t probe_line(const struct vt_line *l,char *dst,size_t cap) {
    size_t n=0;int last=-1;
    for(int x=0;x<g_vt.cols;x++){const struct vt_cell *c=&l->cells[x];if(c->width && c->rune!=' ' && !(c->flags&VT_HIDDEN))last=x;}
    for(int x=0;x<=last;x++) {
        const struct vt_cell *c=&l->cells[x];if(!c->width)continue;
        uint32_t runes[3]={c->rune,c->combining[0],c->combining[1]};
        if(c->flags&VT_HIDDEN){runes[0]=' ';runes[1]=runes[2]=0;}
        for(unsigned j=0;j<3;j++)if(runes[j]){char b[4];unsigned k=vt_encode(runes[j],b);if(n+k>cap)return n;memcpy(dst+n,b,k);n+=k;}
    }
    return n;
}
static uint32_t probe_screen(const struct vt_screen *s,char *dst) {
    size_t n=0;
    for(int y=0;y<g_vt.rows;y++) {
        if(y){if(n+1>=TERM_PROBE_TEXT)break;dst[n++]='\n';}
        n+=probe_line(&s->lines[s->order[y]],dst+n,TERM_PROBE_TEXT-1-n);
    }
    dst[n]=0;return (uint32_t)n;
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
    p->prompt_serial=g_vt.prompt_serial;p->history_count=g_vt.history_count;p->frames=g_frames;
    p->input_len=g_in_n;p->caret=g_cur;memcpy(p->input,g_in,sizeof g_in);
    p->flags=(g_local?TERM_PROBE_LOCAL_EDIT:0)|(g_vt.edit_shell?TERM_PROBE_EDIT_SHELL:0)|(g_vt.inband_resize?TERM_PROBE_INBAND_SIZE:0);
    snprintf(p->cwd,sizeof p->cwd,"%s",g_cwd);
    p->text_len=probe_screen(s,p->text);p->primary_len=probe_screen(&g_vt.primary,p->primary);
    size_t cells=0;
    for(int y=0;y<g_vt.rows;y++)for(int x=0;x<g_vt.cols && cells<TERM_PROBE_CELLS;x++,cells++) {
        const struct vt_cell *c=&s->lines[s->order[y]].cells[x];
        p->cells[cells]=(struct term_probe_cell){c->rune,c->fg,c->bg,c->width,c->flags,{0,0}};
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&p->serial,p->serial+1,__ATOMIC_RELEASE);
}

/* --- runtime hooks -------------------------------------------------------- */

/* idle: drain the terminal master without blocking; wake the renderer
 * only when something actually arrived. Also notice the shell dying. */
static void term_idle(void) {
    bool got = false;
    term_flush();
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
        if(g_view>0)g_view+=(int)(g_vt.scrolls-scrolled);
        got = true; budget-=(size_t)r;
    }
    static unsigned prompt_serial;
    if(prompt_serial!=g_vt.prompt_serial) {
        prompt_serial=g_vt.prompt_serial;g_at_prompt=g_vt.shell_ready;g_local=g_at_prompt && !g_vt.edit_shell;
        snprintf(g_cwd,sizeof g_cwd,"%.*s",(int)sizeof g_cwd-1,g_vt.directory);got=true;
    }
    if (!g_dead && g_shell >= 0 && !embk_proc_alive(g_shell)) {
        g_dead = true;
        term_say("\n[shell exited -- press ESC to close]\n");
        got = true;
    }
    if (got) {probe_refresh();em_request_frame();}
}

static void term_copy(void) {
    static char copy[65536];size_t n=0;
    struct vt *snapshot=NULL;const struct vt *source=&g_vt;
    if(g_local && !g_view) {
        snapshot=malloc(sizeof *snapshot);
        if(!snapshot){snprintf(g_notice,sizeof g_notice,"Screen text could not be copied.");em_request_frame();return;}
        *snapshot=g_vt;snapshot->reply=NULL;source=snapshot;
        vt_feed(snapshot,prompt_text(),strlen(prompt_text()));vt_feed(snapshot," ",1);vt_feed(snapshot,g_in,(size_t)g_in_n);
    }
    for(int y=0;y<g_rows;y++) {
        char row[VT_COLS*12+1];size_t k=vt_line_text(source,y,(unsigned)g_view,row,sizeof row);
        if(n+k+1>sizeof copy){free(snapshot);snprintf(g_notice,sizeof g_notice,"Screen text exceeds the clipboard limit.");em_request_frame();return;}
        memcpy(copy+n,row,k);n+=k;if(y+1<g_rows)copy[n++]='\n';
    }
    free(snapshot);
    if(embk_clip_set(copy,n)<0)snprintf(g_notice,sizeof g_notice,"Screen text could not be copied.");
    else snprintf(g_notice,sizeof g_notice,"Screen copied");
    em_request_frame();
}
static void term_paste(void) {
    if(g_dead)return;
    static char clipboard[65536],paste[65568];
    int64_t held=embk_clip_get(clipboard,sizeof clipboard);
    if(held<=0)return;
    if(held>(int64_t)sizeof clipboard){snprintf(g_notice,sizeof g_notice,"Clipboard text is too large to paste.");em_request_frame();return;}
    bool bracketed=!g_local && g_vt.bracketed_paste;
    size_t n=0;if(bracketed){memcpy(paste,"\033[200~",6);n=6;}
    for(int64_t i=0;i<held;i++) {
        unsigned char c=(unsigned char)clipboard[i];
        if(c=='\r') {if(i+1<held && clipboard[i+1]=='\n')continue;c='\n';}
        if(!bracketed && (c=='\n' || c=='\t'))c=' ';
        if(c==127 || (c<32 && c!='\n' && c!='\t'))continue;
        paste[n++]=(char)c;
    }
    if(bracketed){memcpy(paste+n,"\033[201~",6);n+=6;}
    if(g_local) {
        if(n>IN_MAX-1-(size_t)g_in_n)snprintf(g_notice,sizeof g_notice,"Pasted text exceeds the command line limit.");
        else {memmove(g_in+g_cur+n,g_in+g_cur,(size_t)(g_in_n-g_cur)+1);memcpy(g_in+g_cur,paste,n);g_cur+=(int)n;g_in_n+=(int)n;}
    } else (void)term_send(paste,n);
    em_request_frame();
}

/* --- keys ---------------------------------------------------------------------
 * The kernel queue says which key produced each byte (em_key_code): Up is not
 * Ctrl+S here although both are 0x13, Backspace is not Ctrl+H, and F5 arrives
 * at all. key_identity() names the key the way the screen model's encoder does,
 * or returns 0 for a byte that is simply text. */
static unsigned key_identity(int c,unsigned code,unsigned flags) {
    if(c&EM_KEY_ONLY) {
        code=(unsigned)c&0xffff;
        if(code>=EMBK_KC_F1 && code<=EMBK_KC_F12)return VT_KEY_F1+(code-EMBK_KC_F1);
        return code==EMBK_KC_INS?VT_KEY_INSERT:code==EMBK_KC_MENU?VT_KEY_MENU:0;
    }
    switch(code) {
    case EMBK_KC_UP:return VT_KEY_UP;case EMBK_KC_DOWN:return VT_KEY_DOWN;
    case EMBK_KC_LEFT:return VT_KEY_LEFT;case EMBK_KC_RIGHT:return VT_KEY_RIGHT;
    case EMBK_KC_HOME:return VT_KEY_HOME;case EMBK_KC_END:return VT_KEY_END;
    case EMBK_KC_PGUP:return VT_KEY_PAGEUP;case EMBK_KC_PGDN:return VT_KEY_PAGEDOWN;
    case EMBK_KC_DEL:return VT_KEY_DELETE;
    }
    if(flags&EMBK_KEYF_KEYPAD) {
        if(code>='0' && code<='9')return VT_KEY_KP_0+(code-'0');
        switch(code){case '.':return VT_KEY_KP_DOT;case '\n':return VT_KEY_KP_ENTER;case '+':return VT_KEY_KP_PLUS;
            case '-':return VT_KEY_KP_MINUS;case '*':return VT_KEY_KP_STAR;case '/':return VT_KEY_KP_SLASH;}
    }
    switch(code){case '\b':return VT_KEY_BACKSPACE;case '\n':return VT_KEY_ENTER;case 27:return VT_KEY_ESCAPE;case '\t':return VT_KEY_TAB;}
    return 0;
}
/* Ctrl with a key that is not a letter: the C0 bytes xterm sends for them. */
static int control_byte(unsigned code) {
    switch(code) {
    case ' ':case '2':case '@':return 0;case '[':case '3':return 27;case '\\':case '4':return 28;
    case ']':case '5':return 29;case '^':case '6':return 30;case '-':case '_':case '/':case '7':return 31;
    case '8':case '?':return 127;
    }
    return -1;
}
static size_t encode_key(int c,unsigned key,unsigned code,unsigned mods,char out[16]) {
    unsigned vtmods=((mods&EMBK_KM_SHIFT)?1:0)|((mods&EMBK_KM_ALT)?2:0)|((mods&EMBK_KM_CTRL)?4:0);
    if(key)return vt_key_sequence(&g_vt,key,vtmods,out);
    if(c&EM_KEY_ONLY)return 0;
    if(!code){out[0]=(char)c;return 1;}             /* a character's later UTF-8 byte */
    int byte=c;
    if((mods&EMBK_KM_CTRL) && byte>=0x20 && byte<0x7f){int ctl=control_byte(code);if(ctl>=0)byte=ctl;}
    return vt_key_sequence(&g_vt,(unsigned)byte,vtmods&2,out);
}

/* At the local prompt the keys edit the line, emacs-style; while a program
 * runs they are encoded for it. PgUp/PgDn (Shift+ while a program runs) and the
 * wheel page the scrollback; ESC clears the line first and only closes the
 * window when there is nothing to clear. GUI shortcuts never reach a program. */
static int term_key(int c) {
    unsigned mods=(unsigned)em_key_modifiers(),code=(unsigned)em_key_code(),flags=(unsigned)em_key_flags();g_notice[0]=0;
    unsigned key=key_identity(c,code,flags);
    if(c==EMBK_KEY_COPY || (c==3 && (mods&(EMBK_KM_CTRL|EMBK_KM_SHIFT))==(EMBK_KM_CTRL|EMBK_KM_SHIFT))) {term_copy();return 1;}
    if(c==EMBK_KEY_PASTE || (c==22 && (g_local || (mods&EMBK_KM_SHIFT)))) {term_paste();return 1;}
    if((key==VT_KEY_PAGEUP || key==VT_KEY_PAGEDOWN) && (g_local || (mods&EMBK_KM_SHIFT))) {
        if(!g_vt.alt) {
            g_view+=(key==VT_KEY_PAGEUP?1:-1)*(g_rows/2);
            if(g_view<0)g_view=0;
            if(g_view>(int)g_vt.history_count)g_view=(int)g_vt.history_count;
        }
        em_request_frame();return 1;
    }
    if(mods&EMBK_KM_GUI)return 1;
    if (!g_local && !g_dead) {
        char sequence[16];size_t n=encode_key(c,key,code,mods,sequence);
        if(n)(void)term_send(sequence,n);
        if(g_view)g_view=0;
        em_request_frame();return 1;
    }
    if (key==VT_KEY_ESCAPE) {
        if (g_dead || g_in_n == 0) return 0;     /* nothing to clear -> close */
        g_in[0] = 0; g_in_n = 0; g_cur = 0; g_hist_at = -1;
        em_request_frame();
        return 1;
    }
    if (g_dead) return 1;
    if (key>=VT_KEY_KP_0 && key<=VT_KEY_KP_SLASH && key!=VT_KEY_KP_ENTER) key=0;   /* keypad text */

    switch (key ? key : (unsigned)c) {
    case VT_KEY_UP:   case 0x10: hist_step(-1); break;   /* Up / Ctrl+P */
    case VT_KEY_DOWN: case 0x0E: hist_step(+1); break;   /* Down / Ctrl+N */
    case VT_KEY_LEFT: case 0x02: if(g_cur>0){g_cur--;while(g_cur>0 && ((unsigned char)g_in[g_cur]&0xc0)==0x80)g_cur--;}break;
    case VT_KEY_RIGHT:case 0x06: if(g_cur<g_in_n){g_cur++;while(g_cur<g_in_n && ((unsigned char)g_in[g_cur]&0xc0)==0x80)g_cur++;}break;
    case VT_KEY_HOME: case 0x01: g_cur = 0;      break;  /* Home / Ctrl+A */
    case VT_KEY_END:  case 0x05: g_cur = g_in_n; break;  /* End  / Ctrl+E */
    case 0x03:                                   /* Ctrl+C: abandon the line */
        term_say(prompt_text()); term_putc(' ');
        term_say(g_in); term_say("^C\n");
        g_in[0] = 0; g_in_n = 0; g_cur = 0; g_hist_at = -1;
        break;
    case 0x04:
        if (!g_in_n && term_send("\004",1)) g_at_prompt=g_local=false;
        break;
    case 0x0C:                                   /* Ctrl+L: clear the screen */
        term_clear();
        break;
    case 0x15: in_kill_to_start(); break;        /* Ctrl+U */
    case 0x0B: in_kill_to_end();   break;        /* Ctrl+K */
    case 0x17: in_kill_word();     break;        /* Ctrl+W */
    case VT_KEY_TAB: complete();   break;
    case VT_KEY_BACKSPACE: in_backspace(); break;
    case VT_KEY_DELETE: in_delete(); break;
    case VT_KEY_ENTER: case VT_KEY_KP_ENTER: {
        if (g_view != 0) g_view = 0;
        /* Send the whole line at once. The shell echoes it after our written
         * prompt, completing the transcript line into "user@host:~$ ls" ahead
         * of the output -- the record of what actually ran. */
        hist_push_line(g_in);
        g_hist_at = -1;
        term_say(prompt_text());
        term_putc(' ');
        char command[IN_MAX+1];
        memcpy(command,g_in,(size_t)g_in_n); command[g_in_n]='\n';
        if (!term_send(command,(size_t)g_in_n+1)) return 1;
        g_at_prompt=g_local=false;
        g_in[0] = 0; g_in_n = 0; g_cur = 0;
        break;
    }
    default:
        if (!key && c >= 0x20 && c <= 0xF4) {
            if (g_view != 0) g_view = 0;         /* typing = snap back to live */
            in_insert((char)c);
        }
        break;
    }
    em_request_frame();
    return 1;                                    /* never reaches the toolkit */
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
     * and the shell's crt0 seeds cwd from it. TERM=emlink is the colour
     * contract: it tells the shell someone on the other end parses SGR. */
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/";
    char home_env[160], pwd_env[160];
    snprintf(home_env, sizeof home_env, "HOME=%s", home);
    snprintf(pwd_env, sizeof pwd_env, "PWD=%s", home);
    char user_env[64];
    snprintf(user_env, sizeof user_env, "USER=%s",
             getenv("USER") ? getenv("USER") : "user");
    char *senv[] = { home_env, pwd_env, user_env, "TERM=emlink", "TERM_PROGRAM=EmbLinkTerminal", "EMBLINK_PROMPT=1", NULL };
    int64_t h = embk_spawn_env("/system/bin/shell.elf", argv, senv, acts, 3);

    close(pair[1]);
    if (h<0) { close(g_master); g_master=-1; return false; }
    g_shell=(int)h; return true;
failed:
    close(pair[0]); close(pair[1]); g_master=-1; return false;
}

/* --- the view -------------------------------------------------------------- */

/* --- a single retained bitmap, painted only when the model/input changes --- */
static uint32_t *g_pixels[2];
static int g_pixel_width,g_pixel_height,g_pixel_index,g_cell_width,g_cell_height;
static uint64_t g_painted_signature;
static bool g_bitmap_valid;

static uint32_t rgb(Color c) {
    return ((uint32_t)(c.r*255+.5f)<<16)|((uint32_t)(c.g*255+.5f)<<8)|(uint32_t)(c.b*255+.5f);
}
static Color colour(uint32_t c) {return (Color){((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,1};}
static void fill(uint32_t *pixels,int x,int y,int w,int h,uint32_t color) {
    int x0=x<0?0:x,y0=y<0?0:y,x1=x+w>g_pixel_width?g_pixel_width:x+w,y1=y+h>g_pixel_height?g_pixel_height:y+h;
    for(int row=y0;row<y1;row++)for(int col=x0;col<x1;col++)pixels[(size_t)row*g_pixel_width+col]=0xff000000u|color;
}
static void paint_cell(struct render_target *rt,int x,int y,const struct vt_cell *c,uint32_t foreground,uint32_t background,uint32_t font,uint32_t bold,float size) {
    if (!c->width) return;
    uint32_t fg=c->fg==VT_DEFAULT?foreground:c->fg,bg=c->bg==VT_DEFAULT?background:c->bg;
    if(c->flags&VT_INVERSE){uint32_t tmp=fg;fg=bg;bg=tmp;}
    if(c->flags&VT_DIM)fg=((fg&0xfefefeu)>>1);
    int px=x*g_cell_width,py=y*g_cell_height,w=c->width*g_cell_width;
    fill(rt->pixels,px,py,w,g_cell_height,bg);
    if(!(c->flags&VT_HIDDEN)) {
        char text[16];unsigned n=0;
        n+=vt_encode(c->rune,text+n);
        for(unsigned i=0;i<2;i++)if(c->combining[i])n+=vt_encode(c->combining[i],text+n);
        text[n]=0;
        cpu_backend_get()->draw_text(rt,(float)px,(float)py+1,text,(c->flags&VT_BOLD)?bold:font,size,colour(fg),1,NULL,(float)w,(float)g_cell_height,false);
        if(c->flags&VT_UNDERLINE)fill(rt->pixels,px,py+g_cell_height-2,w,1,fg);
        if(c->flags&VT_STRIKE)fill(rt->pixels,px,py+g_cell_height/2,w,1,fg);
    }
}
static unsigned next_rune(const char *s,uint32_t *rune) {
    int n=font_utf8_decode(s,rune);return n>0?(unsigned)n:1;
}
static void overlay_text(struct render_target *rt,const char *text,int *x,int *y,uint32_t fg,uint32_t bg,uint32_t font,uint32_t bold,float size) {
    for(const char *p=text;*p;) {
        uint32_t rune;unsigned n=next_rune(p,&rune);p+=n;int w=vt_width(rune);if(w<0)continue;
        if(w==0)continue;
        if(*x+w>g_cols){*x=0;(*y)++;}
        if(*y>=g_rows)break;
        struct vt_cell c={.rune=rune,.fg=fg,.bg=bg,.width=(uint8_t)w};
        paint_cell(rt,*x,*y,&c,fg,bg,font,bold,size);*x+=w;
    }
}
static void paint_cursor(struct render_target *rt,int x,int y,uint32_t color) {
    if(x>=g_cols){x=0;y++;}if(y>=g_rows)return;
    int style=g_local?5:g_vt.cursor_style;
    if(style==3 || style==4)fill(rt->pixels,x*g_cell_width,(y+1)*g_cell_height-2,g_cell_width,2,color);
    else if(style==5 || style==6)fill(rt->pixels,x*g_cell_width,y*g_cell_height,2,g_cell_height,color);
    else {
        /* An outline keeps the underlying character readable without blink. */
        fill(rt->pixels,x*g_cell_width,y*g_cell_height,g_cell_width,1,color);
        fill(rt->pixels,x*g_cell_width,(y+1)*g_cell_height-1,g_cell_width,1,color);
        fill(rt->pixels,x*g_cell_width,y*g_cell_height,1,g_cell_height,color);
        fill(rt->pixels,(x+1)*g_cell_width-1,y*g_cell_height,1,g_cell_height,color);
    }
}
static uint64_t signature(const char *s,uint64_t h) {for(;*s;s++)h=(h^(unsigned char)*s)*1099511628211ull;return h;}
static bool screen_bitmap(void) {
    const struct ui_theme *t=ui_theme();
    uint32_t font=em_font("/system/fonts/mono.ttf"),bold=em_font("/system/fonts/mono-bold.ttf");
    if(!font)font=t->font_regular;
    if(!bold)bold=font;
    float size=t->text_caption;struct font *f=font_for_handle(font);if(!f)return false;
    struct glyph_cache_entry *m=glyph_cache_lookup_or_rasterize(font_global_atlas(),f,'M',size);
    int cw=m?(int)ceilf(m->advance_px):8;
    int ch=(int)ceilf((f->ascent+f->descent+f->line_gap)*size/f->units_per_em)+2;
    if(cw<1)cw=1;
    if(ch<1)ch=1;
    int cols=(int)((em_viewport_width()-28)/cw),rows=(int)((em_viewport_height()-t->toolbar_height-20)/ch);
    if(cols<2)cols=2;
    if(cols>VT_COLS)cols=VT_COLS;
    if(rows<2)rows=2;
    if(rows>VT_ROWS)rows=VT_ROWS;
    int w=cols*cw,h=rows*ch;
    if(w!=g_pixel_width || h!=g_pixel_height) {
        uint32_t *a=malloc((size_t)w*h*4),*b=malloc((size_t)w*h*4);
        if(!a || !b){free(a);free(b);return false;}
        bool erased=g_vt.inband_resize && vt_prompt_erase(&g_vt);   /* the shell redraws on the report */
        if(g_vt.inband_resize)g_size_report=true;
        if(!vt_resize(&g_vt,cols,rows)){free(a);free(b);return false;}
        if(erased)vt_prompt_here(&g_vt);
        g_vt.pixel_width=w;g_vt.pixel_height=h;
        free(g_pixels[0]);free(g_pixels[1]);g_pixels[0]=a;g_pixels[1]=b;g_pixel_width=w;g_pixel_height=h;g_bitmap_valid=false;
    }
    g_cols=cols;g_rows=rows;g_cell_width=cw;g_cell_height=ch;
    if(g_view>(int)g_vt.history_count)g_view=(int)g_vt.history_count;
    if(g_vt.alt)g_view=0;
    if(g_local && !g_view) {
        unsigned cells=(unsigned)(strlen(prompt_text())+1+strlen(g_in));
        unsigned offset=g_vt.primary.cursor.wrap?(unsigned)g_cols:(unsigned)g_vt.primary.cursor.x;
        vt_prompt_room(&g_vt,(offset+cells)/(unsigned)g_cols);
    }
    uint32_t bg=rgb(t->bg),fg=rgb(t->text),accent=rgb(t->accent);
    uint64_t hash=signature(g_in,g_vt.revision*1099511628211ull);
    hash=signature(prompt_text(),hash);hash^=(uint64_t)g_cur<<32;hash^=(uint64_t)(g_local+2*g_dead+4*g_view)<<24;
    hash^=(uint64_t)bg<<16;hash^=(uint64_t)fg;hash^=(uint64_t)accent<<32;
    hash^=(uint64_t)w<<40;hash^=(uint64_t)h<<48;hash^=(uint64_t)font<<8;hash^=(uint64_t)(size*4);
    if(!g_bitmap_valid || hash!=g_painted_signature) {
        g_pixel_index^=1;struct render_target rt={g_pixels[g_pixel_index],(uint32_t)w,(uint32_t)h,(uint32_t)w*4,EMBK_PIXFMT_BGRA8888_PRE,0};
        struct clip_saved clip;cpu_clip_save_and_clear(&clip);
        fill(rt.pixels,0,0,w,h,bg);
        for(int y=0;y<rows;y++)for(int x=0;x<cols;x++)paint_cell(&rt,x,y,vt_view_cell(&g_vt,y,x,(unsigned)g_view),fg,bg,font,bold,size);
        struct vt_cursor *cursor=&vt_active(&g_vt)->cursor;
        int x=cursor->x,y=cursor->y;
        if(g_local && cursor->wrap){x=0;y++;}
        if(g_local && !g_view) {
            overlay_text(&rt,prompt_text(),&x,&y,accent,bg,bold,bold,size);overlay_text(&rt," ",&x,&y,fg,bg,font,bold,size);
            int caret_x=x,caret_y=y;
            for(int i=0;i<g_cur;) {
                uint32_t rune;unsigned n=next_rune(g_in+i,&rune);i+=(int)n;int width=vt_width(rune);if(width<0)width=1;
                if(caret_x+width>cols){caret_x=0;caret_y++;}caret_x+=width;
            }
            overlay_text(&rt,g_in,&x,&y,fg,bg,font,bold,size);x=caret_x;y=caret_y;
        }
        if(!g_view && !g_dead && (g_local || g_vt.cursor_visible))paint_cursor(&rt,x,y,accent);
        cpu_clip_restore(&clip);g_painted_signature=hash;g_bitmap_valid=true;
        g_frames++;probe_refresh();
    }
    return true;
}
static void term_view(void) {
    const Color bg=ui_theme()->bg;
    Window("Terminal",.background=bg) {
        AppBar(g_notice[0]?g_notice:title_text(),.background=bg) {
            if(Button("Copy screen").ghost().clicked())term_copy();
            if(Button("Paste").ghost().clicked())term_paste();
            if(Button("Clear").ghost().clicked())term_clear();
        }
        VStack(.spacing=0,.px=14,.pt=10,.pb=10,.align=Leading,.clip=1,.background=bg) {
            float wheel=ui_take_wheel();
            if(wheel && !g_vt.alt) {g_view+=(int)(wheel*3);if(g_view<0)g_view=0;if(g_view>(int)g_vt.history_count)g_view=(int)g_vt.history_count;}
            if(screen_bitmap()) {em_flush();ui_image_sized(0x5445524d,g_pixels[g_pixel_index],(uint32_t)g_pixel_width,(uint32_t)g_pixel_height,(float)g_pixel_width,(float)g_pixel_height);}
            else Text("Terminal display could not be resized.").secondary();
        }
        Spacer();
    }
}

int main(void) {
    vt_init(&g_vt,g_cols,g_rows,term_reply,NULL);
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
    free(g_pixels[0]);free(g_pixels[1]);
    return rc;
}
