/* user/apps/notepp/notepp.c -- Note++, the OS's own code editor.
 *
 * user/apps/edit/edit.c is the one-file editor: a buffer, a Save button, the path
 * Files handed it. This is the other thing -- several files at once, a caret
 * you can drive from the keyboard, selection, undo, find and replace, and the
 * code coloured the way this OS's own compiler reads it.
 *
 * WHAT IS HERE AND WHAT IS NOT. Everything that happens to TEXT lives in
 * user/note/edit.c and is covered by 45 host tests, because an editor is
 * mostly edge cases and none of them are worth discovering on a screen. This
 * file is the part that cannot be tested that way: pixels, keys and files.
 *
 * It draws its own text rather than using the toolkit's TextEditor, which
 * cannot show a selection, a current line or a match, and owns the keyboard in
 * a way an editor's shortcuts cannot share.
 *
 * The chrome is the shape Vellum arrived at: the window's lights share the tab
 * strip, because a title bar has nothing to say that the tab does not.
 * Monospace throughout, chosen the way term.c chooses it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "emnotify.h"
#include "emfiles.h"
#include "workspace.h"
#include <errno.h>
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "syntax.h"
#include "doc.h"
#include "edit.h"

/* ---- palette ------------------------------------------------------------ */

static Color rgb(unsigned v) {
    Color c;
    c.r = (float)((v >> 16) & 0xFF) / 255.0f;
    c.g = (float)((v >>  8) & 0xFF) / 255.0f;
    c.b = (float)( v        & 0xFF) / 255.0f;
    c.a = (float)((v >> 24) & 0xFF) / 255.0f;
    return c;
}

/* One colour per syntax role. Stated here rather than taken from the theme: a
 * syntax palette has to keep seven things apart at body size on one
 * background, which is a different problem from making a button look pressed. */
static Color role_color(int role) {
    bool dark=ui_theme()->bg.r<.5f;
    switch(role) {
    case SYN_KEYWORD:return rgb(dark?0xffe8a1c4:0xff83406f);
    case SYN_TYPE:return rgb(dark?0xff8ac8db:0xff196379);
    case SYN_STRING:return rgb(dark?0xffe6b394:0xff8c4a31);
    case SYN_COMMENT:return ui_theme()->text_tertiary;
    case SYN_NUMBER:return rgb(dark?0xffd9c484:0xff765f24);
    case SYN_PREPROC:return rgb(dark?0xffc0a4dc:0xff72509d);
    case SYN_PUNCT:return ui_theme()->text_secondary;
    default:return ui_theme()->text;
    }
}
#define C_GUTTER (ui_theme()->text_tertiary)
#define C_GUTTER_CUR (ui_theme()->text)
#define C_CURLINE (ui_theme()->surface_alt)
#define C_SEL (ui_theme()->selection)
#define C_CARET (ui_theme()->accent)
#define C_EDITOR_BG (ui_theme()->bg)
#define C_MATCH (ui_theme()->accent_soft)
#define C_FIND (ui_theme()->accent_soft)
#define TAB_ON (ui_theme()->surface_alt)
#define TAB_OFF rgb(0)
#define TAB_TEXT_ON (ui_theme()->text)
#define TAB_TEXT_OFF (ui_theme()->text_secondary)

/* ---- state --------------------------------------------------------------- */

/* The height of one line of code, everywhere: the row, the scroller, and the
 * page keys all have to agree or the caret leaves the screen. */
#define NOTE_LINE_H (ui_theme()->text_body+7.f)
/* How much of one line the highlighter is given. Beyond this a line is not
 * coloured past the cap -- it is still all there, still editable, still saved. */
#define NOTE_LINE_MAX 4096

static struct editor ED;          /* the engine, re-pointed at the current doc */
static int   g_bound = -1;        /* which doc slot ED currently wraps         */
static char  g_msg[96];
static char  g_path_field[DOC_PATH_MAX];
static int   g_want_open, g_want_new, g_switch_to = -1, g_close = -1;
static int   g_last_doc = -1;

/* find / replace */
static int   g_find_open, g_replace_open, g_find_icase = 1;
static char  g_find_buf[80], g_repl_buf[80];

static float g_scroll;            /* first visible line                        */
static int   g_hscroll;           /* first visible COLUMN                      */
static int   g_cols = 80;         /* columns that fit, recomputed from the box */
static int   g_rows = 20;         /* visible lines, recomputed from the window */
static int   g_match = -1;        /* bracket matching the caret's, or -1       */
static float g_doc_top;           /* screen y of the first drawn line          */
static float g_char_w;            /* one monospace advance, measured once      */
static float g_gutter_w;          /* the number column, in pixels              */
static int   g_dragging;          /* a selection drag is in progress           */
static int   g_track_drag;        /* the scroll thumb is being dragged         */
static int   g_wrap = 1;          /* word wrap on/off (status bar toggles it)  */
#define WRAP_MAX 64               /* visual rows one logical line may become   */
static int   g_goto_open;
static char  g_goto_buf[16];
/* Frames each sheet has been up. A modal must not honour a click-off on the
 * frame it appears: the press that OPENED it is still being delivered, and a
 * sheet that opens and shuts in the same gesture reads as a button that does
 * nothing. The launcher learned this; these three inherited the bug instead. */
static int   g_goto_frames, g_confirm_frames;
#define SHEET_SETTLED 3
static struct emdoc_workspace g_workspace;
static struct emdoc_stamp g_read_stamp;
static uint64_t g_checkpoint_ms;
static int g_quitting;
static unsigned g_quit_discard_mask;
static int   g_confirm_close = -1;/* a modified document asking before it goes */
static uint64_t g_last_click_ms;  /* for double / triple click                 */
static int   g_click_streak;
static int   g_last_click_off = -1;

/* THE VISUAL-ROW MODEL, forward-declared because the key hook and the scroller
 * are written above it. With wrap on, a screen row is no longer a line: these
 * four are the only place that difference is allowed to live, and everything
 * that used to count lines now counts rows through them. */
static int  vis_total(void);
static int  vis_row_of(int at);
static void move_visual(int dir, int extend);


/* ---- files --------------------------------------------------------------- */

static long file_read(const char *path, char *buf, size_t cap) {
    if(emdoc_read(path,buf,cap,&g_read_stamp))return -1;
    return (long)strlen(buf);
}

/* Point the engine at whichever document is current. The engine holds a
 * pointer INTO the document's buffer rather than a copy, so switching tabs is
 * re-binding and never a save. */
static void bind_current(void) {
    struct doc *d = doc_cur();
    if (!d) return;
    /* Already bound: leave it alone. Copying the document's cursor back in here
     * looked harmless and was not -- the key hook runs BETWEEN frames, so every
     * keystroke moved the engine's caret and the next frame put it back where
     * the document last remembered it. Typing "abc" left the caret at zero with
     * a two-character selection behind it. While a document is bound the ENGINE
     * owns the caret; the document only needs it when switching away. */
    if (g_bound == doc_current()) return;
    /* Hand the outgoing document its history back before the engine is reset;
     * ed_init clears the arena, so anything not saved here is gone. */
    { struct doc *old = doc_at(g_bound);
      if (old && old->open) ed_undo_save(&ED, &old->undo); }
    ed_init(&ED, d->text, DOC_CAP);
    ed_undo_load(&ED, &d->undo);
    ED.cursor = ED.anchor = d->cursor <= ED.len ? d->cursor : 0;
    g_bound = doc_current();
}
static void sync_out(void) {
    struct doc *d = doc_cur();
    if (d) d->cursor = ED.cursor;
}

static void session_save(void);   /* defined below; save records the session */

static const char *document_parent(const char *path) {
    static char dir[256];
    if(!path[0]) {const char *h=getenv("HOME");return h&&h[0]?h:"/home";}
    snprintf(dir,sizeof dir,"%s",path);char *slash=strrchr(dir,'/');
    if(slash==dir)slash[1]=0;else if(slash)*slash=0;return dir;
}
static int save_document(int save_as) {
    struct doc *d=doc_cur();if(!d)return 0;
    if(d->truncated){snprintf(g_msg,sizeof g_msg,"Refused: incomplete document");return 0;}
    char path[DOC_PATH_MAX];snprintf(path,sizeof path,"%s",d->path);
    struct emdoc_stamp expected=d->original;
    if(save_as || !path[0]) {
        const char *base=strrchr(d->path,'/');base=base?base+1:(d->path[0]?d->path:"Untitled.txt");
        int r=embk_file_panel(EMFILE_SAVE,"Save document",document_parent(d->path),base,path,sizeof path);
        if(r<=0){if(r<0)snprintf(g_msg,sizeof g_msg,"Save dialog unavailable (%d)",r);return 0;}
        if(emdoc_probe(path,&expected)){snprintf(g_msg,sizeof g_msg,"Cannot inspect destination");return 0;}
        /* Don't create two divergent tabs over the same file through Save As. */
        for(int i=0;i<DOC_MAX;i++)if(i!=doc_current() && doc_at(i) && !strcmp(doc_at(i)->path,path)) {
            snprintf(g_msg,sizeof g_msg,"That file is open in another tab");return 0;
        }
    }
    struct emdoc_stamp saved;
    if(emdoc_save(path,d->text,strlen(d->text),&expected,&saved)) {
        snprintf(g_msg,sizeof g_msg,"%s",errno==EAGAIN?"File changed on disk. Use Save As.":"Save failed; changes remain open");return 0;
    }
    snprintf(d->path,sizeof d->path,"%s",path);d->original=saved;d->lang=syn_lang_of(path);
    snprintf(g_path_field,sizeof g_path_field,"%s",path);doc_mark_saved(doc_current());
    emws_forget(&g_workspace,doc_current());session_save();
    snprintf(g_msg,sizeof g_msg,"Saved and synced %lu bytes",(unsigned long)strlen(d->text));
    embk_puts(1,"Note++: document saved and synced\n");return 1;
}
static int save_current(void) {return save_document(0);}
static void pick_begin(void) {
    struct doc *d=doc_cur();char path[DOC_PATH_MAX];
    int r=embk_file_panel(EMFILE_OPEN,"Open document",document_parent(d?d->path:""),0,path,sizeof path);
    if(r==1){snprintf(g_path_field,sizeof g_path_field,"%s",path);g_want_open=1;}
    else if(r<0)snprintf(g_msg,sizeof g_msg,"Open dialog unavailable (%d)",r);
}

/* The token that starts a line comment, per language. Markdown and plain text
 * have none, and a comment toggle that invents one would corrupt the file. */
static const char *comment_tok(enum syn_lang l) {
    switch (l) {
        case SYN_C: case SYN_JS: return "//";
        case SYN_PY: case SYN_SH: return "#";
        default: return 0;
    }
}

static enum syn_lang d_lang(void) {
    struct doc *d = doc_cur();
    return d ? d->lang : SYN_PLAIN;
}

/* ---- the session ---------------------------------------------------------
 * Which files were open, and where the caret was in each. Reopening an editor
 * to an empty Untitled after it was full of work is the friction you meet
 * every single launch, and it is a dozen lines to not have.
 *
 * A flat text file under $HOME rather than anything structured: it is a list of
 * paths, a person may want to read or delete it, and a format nobody can
 * inspect is a format nobody can fix. Written whenever the set of documents
 * changes -- not on every keystroke, which would be a disk write per character
 * for a fact that has not changed. */
static void session_path(char *out, size_t cap) {
    const char *h = getenv("HOME");
    snprintf(out, cap, "%s/.notepp-session", (h && h[0]) ? h : "/data");
}

static void session_save(void) {
    char sp[256]; session_path(sp, sizeof sp);
    char data[DOC_MAX*(DOC_PATH_MAX+48)];size_t at=0;
    for(int i=0;i<DOC_MAX;i++) {
        struct doc *d=doc_at(i);if(!d || !d->path[0])continue;
        int n=snprintf(data+at,sizeof data-at,"%d %d %s\n",i==doc_current(),d->cursor,d->path);
        if(n<0 || (size_t)n>=sizeof data-at)return;
        at+=(size_t)n;
    }
    if(emdoc_save(sp,data,at,0,0))snprintf(g_msg,sizeof g_msg,"Could not save workspace history");
}

static void session_load(void) {
    char sp[256]; session_path(sp, sizeof sp);
    FILE *f = fopen(sp, "r");
    if (!f) return;
    char line[400];
    int want = -1;
    while (fgets(line, sizeof line, f)) {
        int cur = 0, off = 0;
        char path[DOC_PATH_MAX];
        if (sscanf(line, "%d %d %255[^\n]", &cur, &off, path) != 3) continue;
        int i = doc_open(path, file_read);
        if (i < 0) continue;                    /* out of slots: keep what loaded */
        struct doc *d = doc_at(i);
        if(d)d->original=g_read_stamp;
        /* The file may have changed since -- shorter, or gone. Clamp rather
         * than restore a caret past the end of it. */
        if (d && off >= 0 && off <= (int)strlen(d->text)) d->cursor = off;
        if (cur) want = i;
    }
    fclose(f);
    if (want >= 0) doc_select(want);
}

static void checkpoint(void) {
    sync_out();
    for(int i=0;i<DOC_MAX;i++) {
        struct doc *d=doc_at(i);
        if(!d){if(g_workspace.present[i])emws_forget(&g_workspace,i);continue;}
        struct emdoc_draft meta={.cursor=(uint32_t)d->cursor,.original=d->original};
        snprintf(meta.path,sizeof meta.path,"%s",d->path);
        if(emws_checkpoint(&g_workspace,i,&meta,d->text,doc_dirty(i)))
            snprintf(g_msg,sizeof g_msg,"Draft recovery unavailable; save your work");
    }
}
static void idle(void) {
    uint64_t now=embk_uptime_ms();if(now-g_checkpoint_ms<2000)return;
    g_checkpoint_ms=now;checkpoint();
}
static int restore_draft(const struct emdoc_draft *meta,const char *text) {
    struct doc *d=doc_cur();int i=doc_current();
    if(!d || d->path[0] || d->text[0])i=doc_new();
    if(i<0)return -1;
    d=doc_at(i);
    snprintf(d->path,sizeof d->path,"%s",meta->path);strcpy(d->text,text);d->cursor=(int)meta->cursor;
    d->original=meta->original;d->saved_hash=doc_hash(text)^1;d->lang=syn_lang_of(d->path);
    g_bound=-1;return i;
}
static int close_window(void) {
    sync_out();
    for(int i=0;i<DOC_MAX;i++)if(doc_dirty(i) && !(g_quitting && (g_quit_discard_mask&(1u<<i)))) {
        g_quitting=1;g_confirm_close=i;g_confirm_frames=0;checkpoint();em_request_frame();return 0;
    }
    session_save();
    for(int i=0;i<DOC_MAX;i++)emws_forget(&g_workspace,i);
    return 1;
}
static void confirmed_close(int save) {
    int index=g_confirm_close;
    if(save) {
        sync_out();doc_select(index);g_bound=-1;bind_current();
        if(!save_current())return;
    }
    if(g_quitting) {
        /* Saving/discarding one tab advances to the next dirty document. Do
         * not close tabs: the workspace should reopen them after a clean quit. */
        if(!save)g_quit_discard_mask|=1u<<index;
        g_confirm_close=-1;em_app_request_exit(0);
    } else {emws_forget(&g_workspace,index);doc_close(index);g_confirm_close=-1;g_bound=-1;session_save();}
}

/* ---- scrolling ----------------------------------------------------------- */

/* Keep the caret on screen. Called after anything that moves it -- which is
 * why it lives in one place rather than at every call site. */
static void scroll_to_caret(void) {
    int line = vis_row_of(ED.cursor);
    int first = (int)g_scroll;
    if (line < first)                first = line;
    else if (line >= first + g_rows) first = line - g_rows + 1;
    if (first < 0) first = 0;
    g_scroll = (float)first;

    /* ...and sideways. Without this a caret driven past the right edge simply
     * stopped being visible, which is the same class of lie as a line whose
     * tail is silently clipped: the editor knows where the caret is and the
     * screen does not say. A margin of a few columns keeps some context ahead
     * of it rather than pinning it to the very edge. */
    /* Sideways only when NOT wrapped: a wrapped view has nothing off the edge. */
    if (g_wrap) { g_hscroll = 0; return; }
    int col = ed_col_of(&ED, ED.cursor);
    if (col < g_hscroll + 4)            g_hscroll = col - 4;
    else if (col >= g_hscroll + g_cols - 4) g_hscroll = col - g_cols + 5;
    if (g_hscroll < 0) g_hscroll = 0;
}

/* ---- keyboard ------------------------------------------------------------ */

/* The editor owns the keyboard, so every shortcut is decided here. Returning 1
 * means "handled" and stops the toolkit seeing it -- which is how Ctrl+A can
 * mean select-all in the document and still type an 'a' into the find field. */
static int on_key(int ch) {
    if(g_confirm_close>=0) {
        if(ch==27){g_confirm_close=-1;g_quitting=0;g_quit_discard_mask=0;}
        return 1;
    }
    int mods  = em_key_modifiers();
    int ctrl  = (mods & 0x02) != 0;
    int shift = (mods & 0x01) != 0;

    /* A TEXT FIELD HAS THE KEYS WHEN IT HAS FOCUS. The hook sees every key in
     * the window, and an editor's hook handles nearly all of them -- so
     * without this the path field and the find box could not be typed into at
     * all: every character went into the document behind them. Save is the one
     * exception, because Ctrl+S means the same thing wherever you are. */
    if (ui_any_focus()) {
        if (ctrl && ch == 19) { save_current(); return 1; }   /* Ctrl+S */
        if (ch == 27) { g_find_open = g_replace_open = g_goto_open = 0; return 0; }
        return 0;
    }

    /* THE SYSTEM'S EDITING KEYS, which are the GUI key's and not Ctrl's.
     *
     * Ctrl+C cannot be copy in this OS -- 0x03 is the console interrupt and is
     * consumed in the driver whenever something has routed one, so this app's
     * own Ctrl+C below works only while no terminal is running a job. The GUI
     * key has no such problem, and these bytes arrive already decided by the
     * driver, modifiers and all. The Ctrl shortcuts stay exactly as they were;
     * this is an addition, not a replacement.
     *
     * Below the focus check, like every other document shortcut here: with the
     * find box focused these bytes never reach this point and the toolkit's own
     * field handles them, so copying in the find box copies the find box. This
     * is the DOCUMENT's copy. */
    if (ch == EMBK_KEY_SEL_ALL) { ed_select_all(&ED); return 1; }
    if (ch == EMBK_KEY_COPY || ch == EMBK_KEY_CUT) {
        char tmp[4096];
        int n = ed_copy(&ED, tmp, sizeof tmp);
        if (n) { embk_clip_set(tmp, (size_t)n);
                 if (ch == EMBK_KEY_CUT) ed_delete_sel(&ED);
                 snprintf(g_msg, sizeof g_msg, "%d bytes %s", n,
                          ch == EMBK_KEY_CUT ? "cut" : "copied"); }
        return 1;
    }

    /* PASTE, taken raw. The runtime would otherwise replay the clipboard
     * through this hook with newlines flattened to spaces, which is the right
     * rule for a terminal and turns a pasted function into one long line here.
     * Taking 0x16 (Ctrl+V) and EMBK_KEY_PASTE (GUI+V) ourselves keeps the line
     * breaks. */
    if (ch == 0x16 || ch == EMBK_KEY_PASTE) {
        static char clip[8192];
        int64_t n = embk_clip_get(clip, sizeof clip - 1);
        if (n > 0) {
            clip[n] = 0;
            ed_insert_text(&ED, clip, (int)n);
            scroll_to_caret();
            snprintf(g_msg, sizeof g_msg, "%d bytes pasted", (int)n);
        }
        return 1;
    }

    /* A PRINTABLE CHARACTER IS NEVER A SHORTCUT, whatever the modifier keys say.
     * Ctrl+letter arrives as a control code 1..26; a plain byte with Ctrl still
     * physically held is the RUNTIME REPLAYING A PASTE through this hook. Those
     * were being read as shortcuts, so pasting any text containing an 'l' ran
     * delete-line and pasting a 'd' duplicated one -- the paste vanished and
     * the file lost a line instead. */
    if (ctrl && ch >= 32 && ch < 127) ctrl = 0;

    if (ctrl) {
        /* Ctrl+letter arrives as a CONTROL CODE, 1..26 -- which is why the
         * runtime tests for 0x16 to catch Ctrl+V. Masking with |32 turns
         * Ctrl+Z (0x1A) into ':' and matches nothing, so every shortcut here
         * silently did nothing. Decode it back to the letter. */
        int key = (ch >= 1 && ch <= 26) ? ('a' + ch - 1) : (ch | 32);
        switch (key) {
            case 's': save_current(); return 1;
            case 'n': g_want_new = 1; return 1;
            case 'o': pick_begin(); return 1;
            case 'w': g_close = doc_current(); return 1;
            case 'a': ed_select_all(&ED); return 1;
            case 'z': if (shift) ed_redo(&ED); else ed_undo(&ED); scroll_to_caret(); return 1;
            case 'y': ed_redo(&ED); scroll_to_caret(); return 1;
            case 'f': g_find_open = 1; g_replace_open = 0; return 1;
            case 'h': g_find_open = g_replace_open = 1; return 1;
            case 'd': ed_duplicate_line(&ED); scroll_to_caret(); return 1;
            case 'l': ed_delete_line(&ED); scroll_to_caret(); return 1;
            case '/': { const char *tk = comment_tok(d_lang());
                        if (tk) ed_toggle_comment(&ED, tk);
                        scroll_to_caret(); return 1; }
            case 'g': g_goto_open = 1; g_goto_buf[0] = 0; g_goto_frames = 0; return 1;
            case 'c': case 'x': {
                char tmp[4096];
                int n = ed_copy(&ED, tmp, sizeof tmp);
                if (n) { embk_clip_set(tmp, (size_t)n);
                         if (key == 'x') ed_delete_sel(&ED);
                         snprintf(g_msg, sizeof g_msg, "%d bytes %s", n,
                                  key == 'x' ? "cut" : "copied"); }
                return 1; }
            /* 'v' never reaches here: Ctrl+V arrives as 0x16 and is taken
             * above, raw, so the line breaks survive. */
            default: break;
        }
        /* Ctrl + arrows: by word, and by line for up/down. */
        if (ch == 0x11) { ed_move_word(&ED, -1, shift); scroll_to_caret(); return 1; }
        if (ch == 0x12) { ed_move_word(&ED, +1, shift); scroll_to_caret(); return 1; }
        if (ch == 0x13) { ed_move_line(&ED, -1); scroll_to_caret(); return 1; }
        if (ch == 0x14) { ed_move_line(&ED, +1); scroll_to_caret(); return 1; }
        if (ch == 0x02) { ed_doc_start(&ED, shift); scroll_to_caret(); return 1; }
        if (ch == 0x05) { ed_doc_end(&ED, shift);   scroll_to_caret(); return 1; }
        return 1;                      /* swallow unknown Ctrl combinations */
    }

    switch (ch) {
        case 0x11: ed_move(&ED, -1, shift); break;
        case 0x12: ed_move(&ED, +1, shift); break;
        case 0x13: move_visual(-1, shift); break;
        case 0x14: move_visual(+1, shift); break;
        case 0x02: ed_home(&ED, shift); break;
        case 0x05: ed_end(&ED, shift); break;
        case 0x7F: ed_delete(&ED); break;
        case '\b': ed_backspace(&ED); break;
        case '\n': case '\r': ed_newline(&ED); break;
        case '\t': if (shift) ed_outdent(&ED); else ed_indent(&ED); break;
        case 27:   if (g_find_open) { g_find_open = g_replace_open = 0; break; }
                   return 0;           /* Esc with no find bar quits the app */
        default:
            if (ch >= 32 && ch < 127) {
                /* AUTO-CLOSE. An opener brings its partner and leaves the caret
                 * between them. Typing the CLOSER when it is already the next
                 * character just steps over it, so the ordinary case of typing
                 * a whole call never leaves a stray one behind. */
                char close = ed_auto_close((char)ch);
                int over = (ED.cursor < ED.len && ED.buf[ED.cursor] == (char)ch &&
                            (ch == ')' || ch == ']' || ch == '}' ||
                             ch == '"' || ch == '\''));
                if (over)                      ed_move(&ED, +1, 0);
                else if (close && !ed_has_sel(&ED)) {
                    char pair[2] = { (char)ch, close };
                    ed_insert_text(&ED, pair, 2);
                    ED.cursor = ED.anchor = ED.cursor - 1;
                } else                          ed_insert_char(&ED, (char)ch);
            }
            else return 0;
            break;
    }
    scroll_to_caret();
    return 1;
}

/* ---- visual rows ---------------------------------------------------------
 * With wrap off a row IS a line and every one of these is a rename. With it on
 * a logical line becomes one or more VISUAL ROWS, and five things have to agree
 * about that at once -- drawing, click mapping, up/down, scrolling and the
 * scrollbar. They agree by all asking here rather than each doing the
 * arithmetic itself, which is the only version of this that stays correct. */

/* Rows of one logical line, and where each after the first starts. */
static int line_rows(int ls, int le, int *breaks) {
    if (!g_wrap) return 1;
    return ed_wrap_line(ED.buf + ls, le - ls, g_cols, breaks, WRAP_MAX);
}

static int vis_total(void) {
    if (!g_wrap) return ed_line_count(&ED);
    int brk[WRAP_MAX], total = 0, off = 0;
    for (;;) {
        int le = ed_line_end(&ED, off);
        total += line_rows(off, le, brk);
        if (le >= ED.len) break;
        off = le + 1;
    }
    return total;
}

/* Visual row -> the logical line it belongs to, and the offsets of that row's
 * slice. Returns the line's start offset. */
static int vis_row_at(int want, int *row_lo, int *row_hi) {
    int brk[WRAP_MAX], seen = 0, off = 0;
    for (;;) {
        int le = ed_line_end(&ED, off);
        int r  = line_rows(off, le, brk);
        if (want < seen + r) {
            int k = want - seen;
            int lo = (k == 0) ? off : off + brk[k - 1];
            int hi = (k + 1 < r) ? off + brk[k] : le;
            if (row_lo) *row_lo = lo;
            if (row_hi) *row_hi = hi;
            return off;
        }
        seen += r;
        if (le >= ED.len) {                   /* past the end: clamp to the last row */
            if (row_lo) *row_lo = ed_line_start(&ED, ED.len);
            if (row_hi) *row_hi = ED.len;
            return ed_line_start(&ED, ED.len);
        }
        off = le + 1;
    }
}

/* Which visual row an offset sits on, counting from the top of the document. */
static int vis_row_of(int at) {
    int brk[WRAP_MAX], seen = 0, off = 0;
    for (;;) {
        int le = ed_line_end(&ED, off);
        int r  = line_rows(off, le, brk);
        if (at <= le) {
            int k = 0;
            for (int i = 0; i < r - 1; i++) if (at >= off + brk[i]) k = i + 1;
            return seen + k;
        }
        seen += r;
        if (le >= ED.len) return seen ? seen - 1 : 0;
        off = le + 1;
    }
}

/* ---- pointer -> caret ---------------------------------------------------- *
 * The whole reason em_text_width exists: turning a click into a position in
 * the text needs the width of the text, and only the font knows it. Monospace
 * makes the column arithmetic exact rather than a search -- one advance, one
 * character -- which is another reason this editor is monospace. */

static int offset_at_point(float px, float py) {
    if (g_char_w <= 0.0f) return ED.cursor;
    int want = (int)g_scroll + (int)((py - g_doc_top) / NOTE_LINE_H);
    int last = vis_total() - 1;
    if (want < 0) want = 0;
    if (want > last) want = last;
    int lo, hi;
    vis_row_at(want, &lo, &hi);
    /* Round to the NEAREST gap between characters: clicking the right half of a
     * glyph must put the caret after it. The horizontal offset only applies
     * when wrap is off -- a wrapped view never scrolls sideways, because there
     * is nothing off the edge to reach. */
    int col = (g_wrap ? 0 : g_hscroll)
            + (int)(((px - g_gutter_w) / g_char_w) + 0.5f);
    if (col < 0) col = 0;
    if (lo + col > hi) col = hi - lo;
    return lo + col;
}

/* Up and down move by VISUAL row when wrapped: through a wrapped paragraph the
 * caret should walk the rows you can see, not jump the whole paragraph. */
static void move_visual(int dir, int extend) {
    if (!g_wrap) { ed_move_line_v(&ED, dir, extend); return; }
    int lo, hi;
    int row = vis_row_of(ED.cursor);
    vis_row_at(row, &lo, &hi);
    if (ED.goal_col < 0) ED.goal_col = ED.cursor - lo;
    int goal = ED.goal_col;
    int want = row + dir;
    if (want < 0) { ED.cursor = 0; if (!extend) ED.anchor = ED.cursor; return; }
    if (want > vis_total() - 1) { ED.cursor = ED.len; if (!extend) ED.anchor = ED.cursor; return; }
    int nlo, nhi;
    vis_row_at(want, &nlo, &nhi);
    int w = nhi - nlo;
    ED.cursor = nlo + (goal < w ? goal : w);
    if (!extend) ED.anchor = ED.cursor;
}

/* ---- the document view --------------------------------------------------- */

/* One line: gutter number, then the text as coloured spans, with the selection
 * behind it and the caret dropped in at the right offset. */
static void draw_line(int line, int ls, int le, int syn_state_in, enum syn_lang lang,
                      int cur_line, int sel_lo, int sel_hi, int show_num) {
    char tmp[256];
    /* The line's full length is what the HIGHLIGHTER needs -- a string opened
     * near the start decides the colour of everything after it -- but only the
     * visible columns are ever drawn. That is what removes the old 512-byte
     * draw cap: a long line is no longer truncated, it is WINDOWED, and the
     * part off the right edge is reached by scrolling rather than lost. */
    int n = le - ls;
    if (n > NOTE_LINE_MAX) n = NOTE_LINE_MAX;
    int c0 = g_hscroll, c1 = g_hscroll + g_cols;

    /* EmZero, not 0. Zero means "unset" in EmProps and the theme then supplies
     * its own padding -- which on a row of code is a third of a line of air
     * between every line, and is why this looked like a form rather than a
     * file. */
    /* A FIXED ROW HEIGHT, because a line of code is a line of code. Left to
     * size itself the row took the theme's idea of a comfortable control and
     * came out at double the type's line height -- a file with a blank line
     * between every line of it. This is also the number the scroller and
     * PgUp/PgDn count in, so it is stated once, here. */
    HStack(.spacing = EmZero, .align = Center, .px = EmZero, .py = EmZero,
           .height = NOTE_LINE_H, .minh = NOTE_LINE_H, .maxh = NOTE_LINE_H,
           .background = (line == cur_line) ? C_CURLINE : rgb(0)) {
        /* EVERY Text IS FLUSHED IMMEDIATELY, and that is not a style choice.
         * The DSL STAGES a leaf and emits it on the next one, so a Text() given
         * a stack buffer keeps a POINTER to it -- and the next loop iteration
         * overwrites that buffer before the emit happens. Every line drew
         * whatever happened to be in `tmp` at flush time: line numbers came out
         * as boxes and words appeared on the wrong lines. */
        /* Only the FIRST visual row of a line carries its number. Repeating it
         * down a wrapped paragraph would say the line is four lines, which is
         * the one thing a gutter exists to get right. */
        { char num[16];
          if (show_num) snprintf(num, sizeof num, "%4d ", line + 1);
          else          snprintf(num, sizeof num, "%4s ", "");
          Text(num).font(Body).color(line == cur_line ? C_GUTTER_CUR : C_GUTTER);
          em_flush(); }

        /* EVERY MATCH ON THIS LINE, not just the one the caret is on. A find that
         * shows you one hit at a time makes you press Next to discover whether
         * there are others; showing them all answers that by looking. The current
         * hit is the selection, so it stays the brighter of the two. */
        int mlo[48], mhi[48], mn = 0;
        if (g_find_open && g_find_buf[0]) {
            int nl = (int)strlen(g_find_buf);
            for (int m = ed_find(&ED, g_find_buf, ls, g_find_icase, 0);
                 m >= 0 && m < le && mn < 48;
                 m = ed_find(&ED, g_find_buf, m + nl, g_find_icase, 0)) {
                if (m + nl > le) break;
                mlo[mn] = m; mhi[mn] = m + nl; mn++;
            }
        }

        struct syn_span sp[128];
        int st = syn_state_in;
        int ns = (lang == SYN_PLAIN || n <= 0) ? 0
               : syn_line(lang, ED.buf + ls, n, sp, 128, &st);
        if (ns <= 0) { sp[0].len = n; sp[0].role = SYN_TEXT; ns = n > 0 ? 1 : 0; }

        int off = 0;
        for (int k = 0; k < ns; k++) {
            int sl = sp[k].len;
            if (off + sl > n) sl = n - off;
            if (sl <= 0) continue;
            /* Split the span wherever the selection or the caret begins or ends
             * inside it, so both land between characters rather than being
             * rounded to a span boundary. */
            int i = 0;
            while (i < sl) {
                int col = off + i;                    /* monospace: column == byte */
                if (col >= c1) break;                 /* past the right edge */
                int abs = ls + col;
                int in_sel = (sel_lo < sel_hi && abs >= sel_lo && abs < sel_hi);
                int in_hit = 0;
                for (int m = 0; m < mn; m++)
                    if (abs >= mlo[m] && abs < mhi[m]) { in_hit = 1; break; }
                int run = 1;
                while (i + run < sl && col + run < c1) {
                    int a2 = ls + off + i + run;
                    int s2 = (sel_lo < sel_hi && a2 >= sel_lo && a2 < sel_hi);
                    if (s2 != in_sel) break;
                    int h2 = 0;
                    for (int m = 0; m < mn; m++)
                        if (a2 >= mlo[m] && a2 < mhi[m]) { h2 = 1; break; }
                    if (h2 != in_hit) break;
                    if (a2 == ED.cursor) break;
                    run++;
                }
                if (col + run <= c0) { i += run; continue; }   /* left of the window */
                int skip = (col < c0) ? c0 - col : 0;          /* partly left of it */
                if (abs + skip == ED.cursor) { Text("|").font(Body).color(C_CARET); em_flush(); }
                int keep = run - skip;
                if (keep > (int)sizeof tmp - 1) keep = (int)sizeof tmp - 1;
                memcpy(tmp, ED.buf + abs + skip, (size_t)keep); tmp[keep] = 0;
                run = skip + keep;
                Color fg = role_color(sp[k].role);
                /* A run that carries a BACKGROUND is given its exact width. Left to
                 * size itself the tinted box grew to the next sibling, so a
                 * highlighted word painted a bar reaching across the rest of the
                 * line. Monospace makes the right number exact rather than a
                 * measurement: one advance per character. */
                float w = (float)keep * g_char_w;
                if (g_match >= 0 && abs <= g_match && g_match < abs + run)
                    Text(tmp).font(Body).color(fg).bg(C_MATCH).width(w);
                else if (in_sel)
                    Text(tmp).font(Body).color(fg).bg(C_SEL).width(w);
                else if (in_hit)
                    Text(tmp).font(Body).color(fg).bg(C_FIND).width(w);
                else
                    Text(tmp).font(Body).color(fg);
                em_flush();                       /* tmp is about to change */
                i += run;
            }
            off += sl;
        }
        { int ecol = le - ls;
          if (ED.cursor == le && ecol >= c0 && ecol <= c1)
              { Text("|").font(Body).color(C_CARET); em_flush(); } }
        if (!n && ED.cursor != le) { Text(" ").font(Body); em_flush(); }
        Spacer();
    }
}

static void document_view(float height) {
    struct doc *d = doc_cur();
    if (!d) return;
    enum syn_lang lang = d->lang;

    /* How many lines fit. Recomputed every frame so a resize is not a special
     * case, and used by PgUp/PgDn as well as by the scroller. */
    /* Measured once, from the font actually in use, rather than assumed. */
    static float measured_size;
    if(g_char_w<=0 || measured_size!=ui_theme()->text_body) {
        measured_size=ui_theme()->text_body;
        g_char_w=em_text_width("M",measured_size);g_gutter_w=em_text_width("    0 ",measured_size);
    }

    g_rows = (int)(height / NOTE_LINE_H);
    if (g_char_w > 0.0f) {
        g_cols = (int)((em_viewport_width() - g_gutter_w - 12.0f) / g_char_w);
        if (g_cols < 8) g_cols = 8;
    }
    if (g_rows < 1) g_rows = 1;

    int total = vis_total();
    /* THE LAST ROW STOPS AT THE BOTTOM. Clamping to total-1 instead let the
     * wheel push the whole file off the top and leave a screen of empty rows
     * with the document above it -- which looks exactly like a file that was
     * lost. Scroll ends when the end of the file is on screen. */
    int maxfirst = total - g_rows; if (maxfirst < 0) maxfirst = 0;
    int first = (int)g_scroll;
    if (first > maxfirst) first = maxfirst;
    if (first < 0) first = 0;
    if (g_scroll > (float)maxfirst) g_scroll = (float)maxfirst;

    int sel_lo, sel_hi; ed_sel_range(&ED, &sel_lo, &sel_hi);
    int cur_line = ed_line_of(&ED, ED.cursor);

    /* The highlighter's state starts clean and is carried down by the row loop
     * itself, which walks from the top of the file regardless (it has to, to
     * know which visual row is which). A second walk here would advance the
     * same state twice. */
    int st = SYN_ST_NONE;

    /* THE TRACK, sized from what is on screen. Without it the only clue to
     * where you are in a file is the line number, which tells you the position
     * and not the PROPORTION -- line 400 means nothing until you know whether
     * the file has 420 lines or 40000. */
    float thumb = height * (float)g_rows / (float)(total > 0 ? total : 1);
    if (thumb < 24.0f) thumb = 24.0f;
    if (thumb > height) thumb = height;
    float above = (total > g_rows)
                ? (height - thumb) * (float)first / (float)(total - g_rows) : 0.0f;

    HStack(.spacing = EmZero, .align = Fill, .padding = EmZero,
           .height = height, .key = "docrow") {
    VStack(.spacing = EmZero, .align = Fill, .padding = EmZero, .grow = 1,
           .background = C_EDITOR_BG, .clip = 1,
           .height = height, .key = "doc") {
        /* THE WHEEL scrolls the view; the box only takes it while the pointer
         * is over it, so a wheel meant for something else is left alone. */
        float w = ui_take_wheel();
        if (w != 0.0f) {
            g_scroll -= w * 3.0f;
            if (g_scroll < 0) g_scroll = 0;
            if (g_scroll > (float)maxfirst) g_scroll = (float)maxfirst;
            first = (int)g_scroll;
        }
        /* CLICK places the caret, DRAG extends the selection. ui_is_active is
         * true while the button is held on this box, which is exactly the span
         * of a drag -- so press, move and release need no state of their own
         * beyond remembering that one began. */
        { float px, py; ui_pointer_pos(&px, &py);
          int at = offset_at_point(px, py);

          /* DRAGGING is sampled -- it is a state, true for as long as the
           * button is down on this box. */
          int shift_held = (embk_key_mods() & 0x01) != 0;
          if (ui_is_active()) {
              /* SHIFT-CLICK EXTENDS from where the selection already starts,
               * rather than beginning a new one -- the standard way to grow a
               * selection past the edge of the window without dragging. */
              if (!g_dragging) {
                  g_dragging = 1;
                  if (shift_held) ED.cursor = at;
                  else            ED.anchor = ED.cursor = at;
              }
              else if (g_click_streak <= 1) ED.cursor = at;
              em_request_frame();
          } else if (g_dragging) {
              g_dragging = 0;
          }

          /* COUNTING CLICKS is an EDGE, and the toolkit counts them now --
           * ui_take_press_edges. Neither ui_is_active (a state, sampled once a
           * frame) nor ui_consume_click (which remembers only the LAST press)
           * could tell one click from three: three presses fit easily between
           * two frames under an emulator, and the earlier ones were simply
           * never seen. */
          uint64_t press_ms = 0;
          int presses = em_take_clicks(&press_ms);
          if (presses) {
              /* THE PRESSES ARRIVE IN BATCHES, and that is the whole bug. The
               * loop hands over however many edges happened since it last
               * looked -- under an emulator a double click is routinely BOTH
               * presses in one frame -- and adding one per frame turned three
               * rapid clicks into a streak of one, every time.
               *
               * Count what arrived, not how many times we looked. Two edges in
               * one frame IS a double click: they cannot have been further
               * apart than a frame. */
              uint64_t now = press_ms;   /* the PRESS's time, not this frame's */
              int near = (g_last_click_off >= 0 && at >= g_last_click_off - 1
                                                && at <= g_last_click_off + 1);
              if (near && now - g_last_click_ms < 500) g_click_streak += presses;
              else                                     g_click_streak  = presses;
              g_last_click_ms = now;
              g_last_click_off = at;
              /* A shifted click is an extension, never a double click: two of
               * them in a row mean two extensions, not a word. */
              if (shift_held)               g_click_streak = 1;
              else if (g_click_streak >= 3) ed_select_line(&ED, at);
              else if (g_click_streak == 2) ed_select_word(&ED, at);
              em_request_frame();
          }
        }
        g_doc_top = em_viewport_height() - height - 26.0f;
        /* Walk LOGICAL lines and emit VISUAL rows, skipping the rows above the
         * viewport. The highlighter still runs once per logical line: a string
         * opened on the first row decides the colour of the rest of them. */
        int drawn = 0, seen = 0, lnum = 0, woff = 0;
        while (woff <= ED.len && drawn < g_rows) {
            int le = ed_line_end(&ED, woff);
            int brk[WRAP_MAX];
            int rows = line_rows(woff, le, brk);
            for (int k = 0; k < rows; k++) {
                int rlo = (k == 0) ? woff : woff + brk[k - 1];
                int rhi = (k + 1 < rows) ? woff + brk[k] : le;
                /* State is threaded ROW by row, not line by line: a block
                 * comment opened on a wrapped line's first row has to still be
                 * open on its second. Captured BEFORE the scan advances it. */
                int st_in = st;
                if (lang != SYN_PLAIN) {
                    struct syn_span sp2[128];
                    syn_line(lang, ED.buf + rlo, rhi - rlo, sp2, 128, &st);
                }
                if (seen + k < first) continue;      /* above the viewport */
                if (drawn >= g_rows) break;          /* below it */
                draw_line(lnum, rlo, rhi, st_in, lang, cur_line, sel_lo, sel_hi, k == 0);
                drawn++;
            }
            seen += rows;
            lnum++;
            if (le >= ED.len) break;
            woff = le + 1;
        }
        Spacer();
    }
    /* Only when there is something to scroll. A full-height thumb on a short
     * file is a control that says nothing and takes width to say it. */
    if (total > g_rows) {
        VStack(.spacing = EmZero, .align = Fill, .padding = EmZero,
               .width = 10, .height = height, .background = C_EDITOR_BG,
               .key = "track") {
            /* DRAGGABLE. A track that only reports the position is half a
             * control: the thing it draws is exactly the thing you want to
             * grab. The pointer's offset down the track maps straight to the
             * first visible line, with the thumb's own height taken out of the
             * range so the bottom of the travel is the bottom of the file
             * rather than somewhere past it. */
            struct instance_handle th = ui_open();
            if (ui_is_active()) {
                float px, py; ui_pointer_pos(&px, &py);
                float span = height - thumb;
                float rel  = (py - g_doc_top) - thumb * 0.5f;
                if (rel < 0) rel = 0;
                if (rel > span) rel = span;
                int line = (span > 0.5f)
                         ? (int)((rel / span) * (float)(total - g_rows) + 0.5f) : 0;
                if (line < 0) line = 0;
                if (line > total - g_rows) line = total - g_rows;
                g_scroll = (float)line;
                first = line;
                above = (height - thumb) * (float)first / (float)(total - g_rows);
                g_track_drag = 1;
                em_request_frame();
            } else g_track_drag = 0;
            (void)th;
            if (above > 0) { VStack(.height = above, .key = "above") { } }
            VStack(.height = thumb, .corner = 3,
                   .background = g_track_drag ? C_GUTTER_CUR : C_GUTTER,
                   .key = "thumb") { }
            Spacer();
        }
    }
    }
}

/* ---- the app ------------------------------------------------------------- */

static void app(void) {
    /* The editor owns the keyboard; installed once, on the first frame, the
     * way vellum installs its own. */
    static int wired = 0;
    if (!wired) {
        wired = 1;
        em_set_key_hook(on_key);em_set_close_hook(close_window);em_set_idle_hook(idle);
        doc_init();emws_init(&g_workspace,"notepp");
        int recovered=emws_restore(&g_workspace,"notepp",DOC_CAP,restore_draft);
        if(recovered>0) {
            snprintf(g_msg,sizeof g_msg,"Recovered %d unsaved document%s",recovered,recovered==1?"":"s");
            embk_puts(1,"Note++: recovered unsaved workspace\n");
        }
        if(recovered<=0)session_load();          /* before the first bind, so it opens onto a real file */
        g_bound = -1;
    }
    doc_init();
    bind_current();

    /* SOMETHING WAS DROPPED ON THIS WINDOW. Files publishes a "path" when you
     * drag a file out of it; anything else carrying a path works the same,
     * because the type is what is agreed on rather than the application.
     *
     * Checked every frame and answers 1 exactly once, so this costs a syscall
     * per frame and nothing else. */
    {
        struct embk_drop d;
        char dropped[512];
        if (embk_drop_take(&d, dropped, sizeof dropped - 1) == 1 &&
            !strcmp(d.type, "path") && d.len) {
            unsigned n = d.len < sizeof dropped - 1 ? d.len : (unsigned)sizeof dropped - 1;
            dropped[n] = 0;
            if(n>=sizeof g_path_field)snprintf(g_msg,sizeof g_msg,"Dropped path is too long");
            else {memcpy(g_path_field,dropped,n+1);g_want_open=1;}
            /* SAY SO. A file dropped on a window that is behind another one
             * opens where nobody can see it, and a drag that appears to do
             * nothing is a drag people stop trying. The notifier is how any
             * program here speaks from outside its own window. */
            { const char *base = dropped, *c;
              for (c = dropped; *c; c++) if (*c == '/') base = c + 1;
              embk_notify("Note++", base); }
        }
    }

    if (g_want_new)  { sync_out();g_want_new = 0; doc_new(); g_path_field[0] = 0; g_bound = -1; session_save(); }
    if (g_want_open) {
        g_want_open = 0;
        int existing=0;
        for(int k=0;k<DOC_MAX;k++)if(doc_at(k) && !strcmp(doc_at(k)->path,g_path_field))existing=1;
        sync_out();
        int i = doc_open(g_path_field, file_read);
        if (i < 0) snprintf(g_msg, sizeof g_msg, "%s",i==-2?"Cannot open this text file":"No room for another file");
        else { struct doc *dd = doc_at(i);
               if(!existing)dd->original=g_read_stamp;
               snprintf(g_msg, sizeof g_msg, "%s%s", dd->text[0] ? "Opened" : "New file",
                        dd->truncated ? " (TRUNCATED -- will not save)" : "");
               g_bound = -1; session_save(); }
    }
    if (g_switch_to >= 0) { sync_out(); doc_select(g_switch_to); g_switch_to = -1; g_bound = -1; session_save(); }
    if (g_close >= 0) {
        /* ASK BEFORE LOSING EDITS. Closing a modified document used to just
         * take it, and that is the only item on this app's list that costs
         * something you cannot get back. */
        g_quitting=0;g_quit_discard_mask=0;
        if (doc_dirty(g_close)) { g_confirm_close = g_close; g_close = -1; g_confirm_frames = 0; }
        else { doc_close(g_close); g_close = -1; g_bound = -1; session_save(); }
    }
    bind_current();

    struct doc *d = doc_cur();
    if (doc_current() != g_last_doc) {
        g_last_doc = doc_current();
        snprintf(g_path_field, sizeof g_path_field, "%s", d ? d->path : "");
        g_msg[0] = 0;
        g_scroll = 0;
    }
    g_match = ed_match_bracket(&ED, ED.cursor);
    sync_out();

    float chrome = 96.0f + (g_find_open ? 34.0f : 0.0f) + (g_replace_open ? 30.0f : 0.0f);

    Window("Note++") {
        /* ROW 1 -- the open files, and the window's lights. */
        HStack(.spacing = 3, .align = Center, .px = 8, .py = 4, .key = "tabs") {
            WindowControls();
            static const char *KEY[DOC_MAX]  = { "d0","d1","d2","d3","d4","d5","d6","d7" };
            static const char *KEYL[DOC_MAX] = { "l0","l1","l2","l3","l4","l5","l6","l7" };
            static const char *KEYX[DOC_MAX] = { "x0","x1","x2","x3","x4","x5","x6","x7" };
            for (int i = 0; i < DOC_MAX; i++) {
                if (!doc_at(i)) continue;
                int cur = (i == doc_current());
                HStack(.spacing = 0, .align = Center, .px = 3, .corner = 8,
                       .background = cur ? TAB_ON : TAB_OFF, .key = KEY[i]) {
                    EmV tb = Button(doc_label(i)).ghost().font(Caption).py(3).px(7).id(KEYL[i]);
                    tb.color(cur ? TAB_TEXT_ON : TAB_TEXT_OFF);
                    if (tb.clicked() && !cur) g_switch_to = i;
                    if (cur && doc_count() > 1 &&
                        Button("\xc3\x97").ghost().font(Caption).py(3).px(5)
                            .color(TAB_TEXT_OFF).id(KEYX[i]).clicked())
                        g_close = i;
                }
            }
            if (Button("+").ghost().font(Caption).py(3).px(7).id("dnew").clicked()) g_want_new = 1;
            DragHandle(.key = "tabdrag") { }
        }
        Divider("tabsep");

        /* ROW 2 -- the path and what can be done with it. */
        HStack(.spacing = 6, .align = Center, .px = 8, .py = 5, .key = "pathrow") {
            Text(d && d->path[0]?d->path:"Untitled document").caption().secondary();
            Spacer();
            if(Button("New").ghost().font(Caption).py(2).clicked())g_want_new=1;
            if(Button("Open").ghost().font(Caption).py(2).clicked())pick_begin();
            if(Button("Save As").ghost().font(Caption).py(2).clicked())save_document(1);
            if(Button("Save").primary().font(Caption).py(2).clicked())save_current();
        }

        /* THE FIND BAR, only when it is open -- an editor that always shows one
         * has given a row of the file to something you use occasionally. */
        if (g_find_open) {
            Divider("findsep");
            HStack(.spacing = 6, .align = Center, .px = 8, .py = 3, .key = "findrow") {
                Text("Find").caption().tertiary();
                /* Return in the find box means "find the next one" -- the gesture
                 * everybody already has in their fingers. */
                if (TextField(g_find_buf, sizeof g_find_buf, "text to find").submitted()) {
                    int at = ed_find(&ED, g_find_buf, ED.cursor + 1, g_find_icase, 1);
                    if (at >= 0) { ED.anchor = at; ED.cursor = at + (int)strlen(g_find_buf);
                                   scroll_to_caret(); }
                    else snprintf(g_msg, sizeof g_msg, "Not found: %.40s", g_find_buf);
                }
                if (Button("Prev").ghost().font(Caption).py(2).clicked()) {
                    int at = ed_find_prev(&ED, g_find_buf, ED.cursor - 1, g_find_icase);
                    if (at >= 0) { ED.anchor = at; ED.cursor = at + (int)strlen(g_find_buf); scroll_to_caret(); }
                }
                if (Button("Next").ghost().font(Caption).py(2).clicked()) {
                    int at = ed_find(&ED, g_find_buf, ED.cursor + 1, g_find_icase, 1);
                    if (at >= 0) { ED.anchor = at; ED.cursor = at + (int)strlen(g_find_buf); scroll_to_caret(); }
                }
                { char cnt[32];
                  int nm = ed_count(&ED, g_find_buf, g_find_icase);
                  int ix = ed_match_index(&ED, g_find_buf, g_find_icase);
                  if (!g_find_buf[0]) cnt[0] = 0;
                  else if (!nm)       snprintf(cnt, sizeof cnt, "none");
                  else                snprintf(cnt, sizeof cnt, "%d of %d", ix, nm);
                  if (cnt[0]) { Text(cnt).caption().tertiary(); em_flush(); } }
                if (Button(g_find_icase ? "Aa off" : "Aa on").ghost().font(Caption).py(2).clicked())
                    g_find_icase = !g_find_icase;
                if (Button("Done").ghost().font(Caption).py(2).clicked())
                    { g_find_open = g_replace_open = 0; }
            }
        }
        if (g_replace_open) {
            HStack(.spacing = 6, .align = Center, .px = 8, .py = 3, .key = "replrow") {
                Text("With").caption().tertiary();
                TextField(g_repl_buf, sizeof g_repl_buf, "replacement");
                if (Button("Replace").ghost().font(Caption).py(2).clicked())
                    ed_replace(&ED, g_find_buf, g_repl_buf, g_find_icase);
                if (Button("All").ghost().font(Caption).py(2).clicked()) {
                    int n = ed_replace_all(&ED, g_find_buf, g_repl_buf, g_find_icase);
                    snprintf(g_msg, sizeof g_msg, "%d replaced", n);
                }
            }
        }
        Divider("editsep");

        CodeSurface() {document_view(em_viewport_height()-chrome);}

        /* GO TO LINE, as its own question. It used to open the FIND bar and ask
         * for a number that nothing then read -- a prompt that lies about what
         * it will do with the answer is worse than no prompt. */
        if (g_goto_open) {
            g_goto_frames++;
            Overlay() {
                Dialog(.width = 300, .spacing = 12, .padding = 18) {
                    char cap[64];
                    snprintf(cap, sizeof cap, "Go to line (1 - %d)", ed_line_count(&ED));
                    Text(cap).caption().secondary(); em_flush();
                    int go = TextField(g_goto_buf, sizeof g_goto_buf, "line").submitted();
                    HStack(.spacing = 8, .align = Center) {
                        Spacer();
                        if (Button("Cancel").ghost().clicked()) g_goto_open = 0;
                        if (Button("Go").primary().clicked()) go = 1;
                    }
                    if (go) {
                        int n = atoi(g_goto_buf);
                        if (n >= 1) {
                            ed_goto_line(&ED, n);
                            /* Put the line in the MIDDLE rather than at the very
                             * top: you asked to look at it, and a line pinned to
                             * the first row has no context above it. */
                            int first_l = ed_line_of(&ED, ED.cursor) - g_rows / 2;
                            g_scroll = (float)(first_l < 0 ? 0 : first_l);
                            scroll_to_caret();
                        }
                        g_goto_open = 0;
                    }
                }
            }
            if (g_goto_frames >= SHEET_SETTLED && OverlayDismissed()) g_goto_open = 0;
        }

        /* The one question this app asks: a modified document is not closed out
         * from under the person who modified it. */
        if (g_confirm_close >= 0) {
            g_confirm_frames++;
            Overlay() {
                Dialog(.width = 380, .spacing = 14, .padding = 20) {
                    char q[96];
                    snprintf(q, sizeof q, "Close %.32s?", doc_label(g_confirm_close));
                    Text(q).heading(); em_flush();
                    Text("It has unsaved changes.").caption().secondary();
                    HStack(.spacing = 8, .align = Center) {
                        Spacer();
                        if(Button("Cancel").ghost().clicked()){g_confirm_close=-1;g_quitting=0;g_quit_discard_mask=0;}
                        if(Button("Save").primary().clicked())confirmed_close(1);
                        if(Button("Discard").destructive().clicked())confirmed_close(0);
                    }
                }
            }
        }

        Divider("statussep");
        HStack(.spacing = 14, .align = Center, .px = 12, .py = 4, .key = "status") {
            char pos[64];
            snprintf(pos, sizeof pos, "Ln %d, Col %d",
                     ed_line_of(&ED, ED.cursor) + 1, ed_col_of(&ED, ED.cursor) + 1);
            Text(pos).caption().tertiary();
            char meta[64];
            int lo, hi; ed_sel_range(&ED, &lo, &hi);
            snprintf(meta, sizeof meta, "%d lines", ed_line_count(&ED));
            Text(meta).caption().tertiary();
            if (hi > lo) { char s[32]; snprintf(s, sizeof s, "%d selected", hi - lo);
                           Text(s).caption().tertiary(); }
            Text(syn_lang_name(doc_cur() ? doc_cur()->lang : SYN_PLAIN)).caption().tertiary();
            Text(ED.use_tabs ? "Tabs" : "Spaces").caption().tertiary();
            if (Button(g_wrap ? "Wrap" : "No wrap").ghost().font(Caption).py(2)
                    .id("wraptog").clicked()) {
                g_wrap = !g_wrap;
                /* g_scroll is a row index, and the toggle changes what a row
                 * IS -- so the old number means nothing afterwards. Re-anchor
                 * on the caret, centred rather than pinned to the top row: the
                 * line you were looking at should stay where you were looking. */
                int r = vis_row_of(ED.cursor) - g_rows / 2;
                g_scroll = (float)(r > 0 ? r : 0);
                scroll_to_caret();
            }
            if (d && doc_dirty(doc_current())) Text("modified").caption().tertiary();
            Spacer();
            if (g_msg[0]) Text(g_msg).caption().tertiary();
        }
    }
}

EM_APPLICATION {
    .title  = "Note++",
    .min_size = { 540, 340 },
    .size   = { 900, 620 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .font   = "/system/fonts/mono.ttf",
    .view   = app,
};
