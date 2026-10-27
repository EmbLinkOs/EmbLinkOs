/* ui/dsl/em_test.c -- the MULTI-LINE editor's caret, selection and clipboard,
 * on the host.  make em-test  -> exits 0 iff every claim holds.
 *
 * WHY THIS FILE EXISTS AT ALL. ui/kit/kit_test.c covers the single-line field,
 * and user/note/edit_test.c covers the Note app's own buffer -- but
 * em_text_editor, which is the editor every EmUI application actually uses,
 * had no host test of any kind. It was only ever exercised by booting a
 * machine and typing, which is slow enough that in practice it was not
 * exercised at all.
 *
 * The bug that made the case: the day the keyboard driver began sending
 * editing commands as bytes 0xF8..0xFF, this editor's default branch inserted
 * ANY byte >= 0x80 as text -- correct for é and €, catastrophic for these --
 * so GUI+C in Note++ would have typed a garbage character into the document
 * instead of copying. The single-line field's host test caught its half of
 * that in a second. This is the other half. */
#include "ui.h"
#include "scene.h"
#include "layout.h"
#include "kit.h"
#include "theme.h"
#include "em.h"
#include "font.h"
#include "testfont.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

static int g_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
                              else { printf("  ok:   %s\n", (msg)); } } while (0)

static struct scene_arena  SA;
static struct layout_arena LA;

/* A clipboard of our own -- the kit takes one as two function pointers exactly
 * so that copy and cut can be tested here instead of only in QEMU. */
static char g_clip[65537];
static unsigned g_clip_n;
static int clip_set(const char *b, unsigned n) {
    if (n > sizeof g_clip) n = sizeof g_clip;
    memcpy(g_clip, b, n); g_clip_n = n; return 0;
}
static int clip_get(char *b, unsigned cap) {
    unsigned n = g_clip_n < cap ? g_clip_n : cap;
    memcpy(b, g_clip, n); return (int)n;
}
static const char *clip_text(void) {
    static char out[65538];
    memcpy(out, g_clip, g_clip_n); out[g_clip_n] = 0; return out;
}

static char DOC[8192];
static int  CUR;

/* A clock the test winds by hand -- double-click is a real elapsed time, and
 * sleeping in a test is how a test becomes flaky on a loaded machine. */
static uint64_t g_ms = 1000;
static uint64_t fake_clock(void) { return g_ms; }

static int highlights(struct instance_handle h) {
    struct scene_node *n=scene_resolve(&SA,ui_scene_of(h));
    int count=n && n->kind==SCENE_NODE_TEXT && n->data.text.bg.a>0;
    for(struct instance_handle c=ui_first_child(h);c.index;c=ui_next_sibling(c))
        count+=highlights(c);
    return count;
}

static void view(void) { em_text_editor(DOC, sizeof DOC, &CUR, 200); }

static void frame(const char *keys) {
    for (const char *k = keys; k && *k; k++) ui_input_char((unsigned char)*k);
    ui_frame_begin(); view(); ui_frame_end();
    ui_run_layout(400, 300);
}

/* The editor takes focus from a CLICK, so the test has to click it -- there is
 * no autofocus on an editor. A press and a release in two frames, because a
 * click is an edge and this pipeline reports edges. */
static void click_in(void) {
    ui_pointer(100, 100, true);
    frame(NULL);
    ui_pointer(100, 100, false);
    frame(NULL);
}

/* Press at (x,y), then release there: a click that PLACES the caret.
 *
 * IT ADVANCES THE CLOCK FIRST, so that consecutive gestures in this file are
 * separate ones. Without that, every click in the test is inside the
 * double-click window of the one before it and the run climbs forever -- which
 * silently disabled the drags, because a drag deliberately does not move the
 * caret while a multi-click owns the selection. Tests that want a real
 * double-click use click_fast(). */
static void click_fast(float x, float y) {
    ui_pointer(x, y, true);
    frame(NULL);
    ui_pointer(x, y, false);
    frame(NULL);
}
static void click_at(float x, float y) { g_ms += 900; click_fast(x, y); }

/* Press at one point, drag to another, release: a selection. */
static void drag(float x0, float y0, float x1, float y1) {
    g_ms += 900;
    ui_pointer(x0, y0, true);
    frame(NULL);
    ui_pointer(x1, y1, true);
    frame(NULL);
    ui_pointer(x1, y1, false);
    frame(NULL);
}


/* --- a value a widget writes through a pointer -------------------------
 *
 * THE BUG THIS PINS DOWN. A DSL element is STAGED, not emitted: Segmented(...)
 * records what to draw and returns, and the widget only runs -- and only then
 * writes through the int* it was handed -- at the next em_flush(), which is
 * whatever element or closing brace comes after it. So Settings' Keyboard pane,
 *
 *     int pick = g_cfg.keymap, was = pick;
 *     Segmented(names, n, &pick);
 *     if (pick != was) { g_cfg.keymap = pick; embk_kbd_layout(...); }
 *
 * reads `pick` one flush too early, every frame. The write does land later --
 * into a local that is about to be thrown away, and the `if` that would have
 * saved it has already run. The keyboard layout could not be changed from
 * Settings no matter how correct the syscall underneath it was.
 *
 * Worse: a brace-scoped container is a for-loop, so the body's locals die
 * BEFORE the closing em_end_() runs. The flush writes through &pick after pick
 * is gone. This test never leaves a staged element pointing at a dead local --
 * a test that relies on undefined behaviour proves nothing -- so it flushes
 * unconditionally at the end of the block and varies only the thing under
 * test: whether the read happens before or after the widget has decided.
 *
 * Staging is worth keeping -- it is what lets .caption().grow() chain after a
 * call -- so the fix is Sync(), and this states the contract in both
 * directions. */
static const char *const SEGN[3] = { "One", "Two", "Three" };
static int seg_cfg;        /* the persisted setting  -- Settings' g_cfg.keymap */
static int seg_applied;    /* how many times the app acted on a change        */
static int seg_sync;       /* 1 = Sync() before reading, as the fix does      */

static void seg_view(void) {
    VStack(.key = "segpane") {
        int pick = seg_cfg, was = pick;
        Segmented(SEGN, 3, &pick);
        if (seg_sync) Sync();
        if (pick != was) { seg_cfg = pick; seg_applied++; }
        Sync();            /* never let the staged element write into a dead local */
    }
}
static void seg_frame(void) {
    ui_frame_begin(); seg_view(); ui_frame_end();
    ui_run_layout(400, 300);
}
static void seg_press(float x, float y) {
    g_ms += 900;
    ui_pointer(x, y, true);  seg_frame();
    ui_pointer(x, y, false); seg_frame();
}
/* The first x on the control that selects something other than segment 0,
 * found by sweeping rather than by guessing a coordinate: a guess that misses
 * reports the same red as a control that does not work. */
static float seg_x_for_change(void) {
    for (float x = 1; x < 300; x += 1) {
        seg_sync = 1; seg_cfg = 0; seg_applied = 0;
        seg_frame(); seg_frame();
        seg_press(x, 12);
        if (seg_cfg != 0) return x;
    }
    return -1;
}

static int under_clicked;
static void under_frame(void) {
    ui_frame_begin();
    if(Button("Under").width(100).height(30).clicked())under_clicked++;
    em_flush();ui_begin_vstack(0xdec047);
    ui_set_overlay(true);ui_set_layer(3);ui_set_hit_passthrough(true);
    ui_set_size((struct layout_size){.mode=SIZE_FIXED,.fixed_value=120},(struct layout_size){.mode=SIZE_FIXED,.fixed_value=40});
    ui_set_paint((struct paint){.kind=PAINT_SOLID,.solid={1,0,0,1}});
    ui_end_stack();ui_frame_end();ui_run_layout(400,100);
}

/* Exercise the shipped EIC path as well as the font fallback: a hover can
 * change stencil tint on the very frame that captures the image leaf. */
static uint8_t *icon_asset(const char *path,size_t *len) {
    if(strncmp(path,"/system/images/",15))return NULL;
    FILE *f=fopen(path+1,"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    uint8_t *p=malloc(n>0?(size_t)n:1);
    if(n<=0 || !p || fread(p,1,(size_t)n,f)!=(size_t)n){free(p);fclose(f);return NULL;}
    fclose(f);*len=(size_t)n;return p;
}
/* A mover like the compositor's: the window goes where it may (here, never
 * above y = 20, as the bar keeps it), and the runtime is told where it went. */
static int32_t mv_x, mv_y;
static void clamp_mover(int win, int32_t x, int32_t y) {
    if (y < 20) y = 20;
    mv_x = x; mv_y = y;
    em_window_bind(win, x, y);
}
static bool chrome_closed;
static void chrome_frame(void) {
    ui_frame_begin();em_new_frame();
    Window("Chrome regression"){AppBar("Chrome regression"){} Spacer();}
    em_flush();em_tooltip_host();chrome_closed=em_window_closed();
    ui_frame_end();ui_run_layout(400,300);
}

static char modal_text[32];
static bool modal_open,nested_open;
static int accepted;
static struct instance_handle outside_button;
static void modal_frame(void) {
    ui_frame_begin();em_new_frame();
    VStack(.key="focus-regression") {
        em_field_autofocus();TextField(modal_text,sizeof modal_text,"Outside");
        if(Button("Show decision").clicked())modal_open=true;
        em_flush();
        outside_button=ui_next_sibling(ui_first_child(ui_first_child(ui_root())));
        if(modal_open)Overlay() {
            Dialog(.width=300) {
                if(Button("Cancel").clicked())modal_open=false;
                if(Button("More options").clicked())nested_open=true;
                if(Button("Proceed").clicked()){accepted++;modal_open=false;}
                if(nested_open)Overlay() {
                    Dialog(.width=200) {
                        if(Button("Back").clicked())nested_open=false;
                        if(Button("Done").clicked())nested_open=false;
                    }
                }
            }
        }
    }
    em_flush();ui_frame_end();ui_run_layout(400,400);
}
static int menu_action;
static float menu_level=.5f;
static void menu_frame(void) {
    ui_frame_begin();em_new_frame();
    VStack(.key="menu-navigation") {
        MenuBar() {
            Menu("File") {
                if(MenuItem("New"))menu_action=1;
                MenuSeparator();
                if(MenuItem("Open"))menu_action=2;
                if(MenuItem("Close"))menu_action=3;
            }
            Menu("Edit") {
                if(MenuItem("Copy"))menu_action=4;
                if(MenuItem("Paste"))menu_action=5;
            }
            Menu("Tools") {if(Button("Hidden action").clicked())menu_action=6;Slider(&menu_level);}
        }
    }
    em_flush();ui_frame_end();ui_run_layout(500,500);
}
static char context_text[48]="Clipboard survives focus.";
static void context_frame(void) {
    ui_frame_begin();em_new_frame();
    VStack(.key="context-focus") {em_field_autofocus();TextField(context_text,sizeof context_text,"Context");}
    em_flush();ui_frame_end();ui_run_layout(500,400);
}
static void navigation_regressions(void) {
    ui_request_focus(INSTANCE_HANDLE_NULL);modal_text[0]=0;modal_open=nested_open=false;accepted=0;
    modal_frame();modal_frame();ui_input_char('x');ui_input_char('\t');ui_input_char('\n');ui_input_char('!');modal_frame();modal_frame();
    CHECK(modal_open && !strcmp(modal_text,"x") && accepted==0,"opening a dialog consumes its activation without typing into the background");
    for(int i=0;i<8;i++){ui_input_char('\t');modal_frame();modal_frame();}
    ui_input_key('\t',UI_MOD_SHIFT);modal_frame();modal_frame();
    ui_input_char('q');modal_frame();
    CHECK(!strcmp(modal_text,"x") && modal_open,"forward and reverse Tab stay inside the dialog");
    /* Return to the first dialog control, then open its nested decision. */
    ui_focus_edge(false);modal_frame();ui_input_char('\t');ui_input_char('\n');modal_frame();modal_frame();
    CHECK(nested_open,"a nested dialog opens from the focused control");
    ui_input_char('\n');modal_frame();modal_frame();modal_frame();
    CHECK(!nested_open && modal_open,"Return in a nested dialog activates only its own default control");
    ui_input_key('\t',UI_MOD_SHIFT);modal_frame();modal_frame();ui_input_char('\n');modal_frame();modal_frame();modal_frame();
    CHECK(!modal_open && ui_has_focus(outside_button),"closing nested and outer dialogs restores their initiating controls");
    ui_request_focus(INSTANCE_HANDLE_NULL);menu_action=0;menu_frame();menu_frame();
    ui_input_char('\t');menu_frame();ui_input_char(UI_KEY_DOWN);menu_frame();menu_frame();
    CHECK(em_menu_any_open(),"Down opens a focused menu from the keyboard");
    CHECK(em_menu_key(UI_KEY_DOWN),"an open menu consumes Down before app hooks");menu_frame();
    ui_input_char('\n');menu_frame();menu_frame();
    CHECK(menu_action==2 && !em_menu_any_open(),"Down skips separators and Return activates the focused item once");
    ui_input_char(UI_KEY_UP);menu_frame();menu_frame();ui_input_char('\n');menu_frame();menu_frame();
    CHECK(menu_action==3,"Up opens a menu at its final item");
    ui_input_char(UI_KEY_DOWN);menu_frame();menu_frame();
    CHECK(em_menu_key(UI_KEY_RIGHT),"Right walks adjacent menus");menu_frame();menu_frame();
    ui_input_char('\n');menu_frame();menu_frame();
    CHECK(menu_action==4,"switching menus focuses the new menu's first item");
    ui_input_char(UI_KEY_DOWN);menu_frame();menu_frame();
    em_menu_key(UI_KEY_LEFT);ui_input_char('\n');menu_frame();menu_frame();
    CHECK(menu_action==1 && !em_menu_any_open(),"queued menu switching and Return activate only the new menu");
    ui_input_char(UI_KEY_DOWN);menu_frame();menu_frame();
    CHECK(em_menu_key(27),"Escape dismisses a menu without reaching the application");menu_frame();menu_frame();
    CHECK(!em_menu_any_open(),"Escape leaves the menu closed");
    ui_input_char(UI_KEY_DOWN);menu_frame();menu_frame();
    em_menu_key(UI_KEY_LEFT);menu_frame();menu_frame(); /* File -> Tools */
    ui_input_char('\t');menu_frame();menu_frame();
    CHECK(!em_menu_key(UI_KEY_RIGHT),"a focused menu slider retains its horizontal adjustment keys");
    ui_input_char(UI_KEY_RIGHT);menu_frame();menu_frame();
    CHECK(menu_level>.54f && menu_level<.56f,"Right adjusts a menu slider without switching menus");
    em_menu_dismiss();menu_frame();menu_frame();
    ui_request_focus(INSTANCE_HANDLE_NULL);context_frame();context_frame();
    ui_input_char(UI_KEY_SEL_ALL);context_frame();
    em_feed_pointer(50,15,0,1,0,1);context_frame();
    em_feed_pointer(50,15,0,0,0,1);context_frame();context_frame();
    CHECK(em_menu_key(UI_KEY_HOME),"a text context menu retains keyboard ownership after field focus moves");context_frame();
    for(int i=0;i<3;i++){em_menu_key(UI_KEY_DOWN);context_frame();}
    ui_input_char('\n');context_frame();context_frame();context_frame();context_frame();
    CHECK(!strcmp(clip_text(),context_text),"context Copy waits for the original field's focus and selection to return");

}

int main(void) {
    printf("=== em-test: the multi-line editor ===\n");
    scene_arena_init(&SA); layout_arena_init(&LA); ui_init(&SA, &LA);
    ui_theme_use_dark(true);

    /* Without a font every text width is zero, and the editor's column
     * hit-testing would agree with any expectation at all. See ui/testfont.h. */
    static struct testfont TF;
    uint32_t fh = font_load(TF.data, testfont_build(&TF));
    if (!fh) { printf("  FAIL: could not build the test font\n"); return 1; }
    ui_theme_set_fonts(fh, fh);
    ui_clock_provider(fake_clock);
    ui_clipboard_provider(clip_set, clip_get);

    #define LEFT   "\x11"
    #define RIGHT  "\x12"
    #define HOME   "\x02"
    #define END    "\x05"
    #define DEL    "\x7F"
    #define SLEFT  "\xF8"
    #define SRIGHT "\xF9"
    #define SHOME  "\xFA"
    #define SEND   "\xFB"
    #define SALL   "\xFC"
    #define COPY   "\xFD"
    #define CUT    "\xFE"
    #define PASTE  "\xFF"

    DOC[0] = 0; CUR = 0;
    frame(NULL);
    click_in();
    frame("hello");
    CHECK(!strcmp(DOC, "hello"), "the editor takes a click and then takes typing");

    /* THE REGRESSION THIS FILE WAS WRITTEN FOR. Every command byte must be
     * CONSUMED, never inserted: the default branch takes any byte >= 0x80
     * because that is how é arrives, and these are above it. */
    frame(COPY CUT SALL SLEFT SRIGHT SHOME SEND);
    frame(HOME);
    CHECK(!strcmp(DOC, "hello"), "no command byte is ever inserted as text");

    frame(END);
    frame(SHOME COPY);
    CHECK(!strcmp(clip_text(), "hello"), "Shift+Home selects the line and copy takes it");

    frame(END);
    frame("\n");
    frame(PASTE);
    CHECK(!strcmp(DOC, "hello\nhello"), "paste puts it back, on a new line");

    /* A PASTE CARRIES ITS NEWLINES. The runtime's app-wide replay flattens
     * them to spaces, which is right for a terminal and wrong here -- an
     * editor that silently collapses a pasted block onto one line is an
     * editor that eats your text. This one pastes itself. */
    frame(SALL COPY);
    frame(END);
    frame(PASTE);
    CHECK(!strcmp(DOC, "hello\nhellohello\nhello"),
          "a pasted block keeps its newlines instead of being flattened");

    /* One queued command must carry more than both the 32-key input queue
     * and the former 2048-byte editor scratch buffer. */
    memset(g_clip,'x',5000);g_clip[100]='\n';g_clip[2100]='\t';g_clip_n=5000;
    frame(SALL);CHECK(highlights(ui_root())>0,"select-all paints a highlight");frame(PASTE);
    CHECK(highlights(ui_root())==0,"paste removes retained selection highlights");
    CHECK(strlen(DOC)==5000 && DOC[100]=='\n' && DOC[2100]=='\t' && CUR==5000,
          "one paste replaces selection with a long multiline block");
    frame("!");
    CHECK(strlen(DOC)==5001 && DOC[5000]=='!',"paste collapses selection at the new caret");
    memset(g_clip,'z',9000);g_clip_n=9000;
    frame(SALL PASTE);
    CHECK(strlen(DOC)==5001 && DOC[0]=='x',"oversized paste preserves document and selection");
    frame("Q");CHECK(!strcmp(DOC,"Q"),"failed paste preserves selection for the next edit");
    clip_set("a\r\nb\tc\033\001",9);frame(SALL PASTE);
    CHECK(!strcmp(DOC,"a\nb\tc"),"multiline paste normalizes CRLF and filters controls");

    DOC[0] = 0; CUR = 0; g_clip_n = 0;
    frame(NULL);
    frame("abc");
    frame(SLEFT SLEFT);
    frame("Z");
    CHECK(!strcmp(DOC, "aZ"), "typing over a selection replaces it");

    frame(SHOME);
    frame("\b");
    CHECK(DOC[0] == 0, "backspace over a selection removes the whole selection");

    /* A plain arrow collapses to the edge it moves toward. */
    DOC[0] = 0; CUR = 0; frame(NULL);
    frame("abcdef");
    frame(SHOME);                       /* all selected, caret at 0 */
    frame(RIGHT);                       /* collapse to the right edge */
    frame("!");
    CHECK(!strcmp(DOC, "abcdef!"), "a plain arrow collapses the selection to the edge it moves toward");

    frame(SALL);
    frame(LEFT);
    frame("^");
    CHECK(!strcmp(DOC, "^abcdef!"), "and Left collapses it to the other one");

    /* Cut, and a selection spanning a newline. */
    DOC[0] = 0; CUR = 0; frame(NULL);
    frame("one\ntwo");
    frame(SHOME SLEFT SHOME);           /* back across the newline to the start */
    frame(CUT);
    CHECK(!strcmp(clip_text(), "one\ntwo") && DOC[0] == 0,
          "a selection spanning a newline cuts both lines");

    /* Multi-byte characters travel whole: one Shift+Left selects all of an é. */
    DOC[0] = 0; CUR = 0; g_clip_n = 0; frame(NULL);
    frame("caf\xC3\xA9");
    frame(SLEFT COPY);
    CHECK(!strcmp(clip_text(), "\xC3\xA9"), "a selection over é carries BOTH its bytes");

    /* ---- THE MOUSE: placing the caret, and dragging a selection ---------
     *
     * Before this the editor answered a click by taking focus and nothing
     * else, so the caret stayed wherever it was and the only way to reach a
     * line was to arrow to it -- in a CODE EDITOR.
     *
     * The y of a line is the surface's top padding plus the line pitch, which
     * is the drawn text size plus the 5px the lines are spaced by. Computed
     * from the theme rather than hardcoded, so this test does not quietly
     * become a test of one font size. */
    {
        const struct ui_theme *th = ui_theme();
        float lh = th->text_body + 5.0f;
        #define LINE_Y(n) (th->sp2 + lh * (n) + lh * 0.5f)

        DOC[0] = 0; CUR = 0; g_clip_n = 0;
        frame(NULL);
        strcpy(DOC, "alpha\nbravo\ncharlie");
        frame(NULL);

        /* Now that there IS a font, a column can be aimed at exactly: every
         * character is half the text size wide, and the text starts one left
         * padding in. */
        float adv = testfont_advance(th->text_body);
        #define COL_X(n) (th->sp3 + adv * (float)(n) + adv * 0.5f)

        click_at(0, LINE_Y(1));
        CHECK(CUR == 6, "a click lands on the line it was aimed at");
        frame("X");
        CHECK(!strcmp(DOC, "alpha\nXbravo\ncharlie"), "and typing goes there");

        strcpy(DOC, "alpha\nbravo\ncharlie"); CUR = 0; frame(NULL);
        click_at(COL_X(2), LINE_Y(1));
        CHECK(CUR == 6 + 3, "and on the COLUMN it was aimed at");
        click_at(COL_X(2), LINE_Y(2));
        CHECK(CUR == 12 + 3, "column and line together");

        /* Far right of a line is its END, not the start of the next one. */
        click_at(390, LINE_Y(0));
        CHECK(CUR == 5, "clicking past the end of a line stops at the end of it");

        /* Below the last line is the last line, not nothing. */
        click_at(390, LINE_Y(9));
        CHECK(CUR == (int)strlen(DOC), "clicking below the text lands at the very end");

        /* A DRAG SELECTS. Pointer capture means it keeps extending even once
         * the pointer has left the editor, which is why the release here is
         * far outside it. */
        strcpy(DOC, "alpha\nbravo\ncharlie"); CUR = 0; frame(NULL);
        drag(0, LINE_Y(0), 390, LINE_Y(1));
        frame(COPY);
        CHECK(!strcmp(clip_text(), "alpha\nbravo"), "dragging across two lines selects both");

        strcpy(DOC, "alpha\nbravo\ncharlie"); CUR = 0; g_clip_n = 0; frame(NULL);
        drag(0, LINE_Y(0), 900, LINE_Y(40));
        frame(COPY);
        CHECK(!strcmp(clip_text(), "alpha\nbravo\ncharlie"),
              "a drag that leaves the editor keeps going to the end");

        /* And the selection a drag made is a real one: typing replaces it. */
        frame("!");
        CHECK(!strcmp(DOC, "!"), "typing replaces what the drag selected");

        /* Double-click takes a word; a third takes the LINE, not the whole
         * document -- a triple-click in a file that selected everything would
         * be a nasty surprise. */
        strcpy(DOC, "alpha bravo\ncharlie"); CUR = 0; g_clip_n = 0; frame(NULL);
        g_ms += 900;  click_fast(COL_X(8), LINE_Y(0));
        g_ms += 50;   click_fast(COL_X(8), LINE_Y(0));
        frame(COPY);
        CHECK(!strcmp(clip_text(), "bravo"), "a double-click in the editor takes the word");

        g_ms += 50;   click_fast(COL_X(8), LINE_Y(0));
        frame(COPY);
        CHECK(!strcmp(clip_text(), "alpha bravo"),
              "a third click takes the LINE, not the whole document");
        #undef COL_X
        #undef LINE_Y
    }

    /* ---- UNDO IN THE EDITOR --------------------------------------------
     *
     * Shares the kit's log rather than growing a second one: two undo stacks in
     * one toolkit is one more than anyone wants to reason about when they
     * disagree. The editor records the same way the field does -- by diffing
     * the document across a frame -- because it mutates `buf` through five
     * different helpers and a log that must be told about each would miss the
     * sixth. */
    #define UNDO "\xF6"
    #define REDO "\xF7"

    DOC[0] = 0; CUR = 0; g_clip_n = 0; g_ms += 5000;
    frame(NULL); frame(NULL);
    click_at(0, 20);
    frame("alpha");
    g_ms += 5000;
    frame("\nbravo");
    CHECK(!strcmp(DOC, "alpha\nbravo"), "two lines typed");
    frame(UNDO);
    CHECK(!strcmp(DOC, "alpha"), "undo takes back the second line");
    frame(REDO);
    CHECK(!strcmp(DOC, "alpha\nbravo"), "and redo puts it back");

    /* THE CASE THE FIELD'S 64-BYTE RECORDS COULD NOT HOLD, and the reason the
     * log moved to an arena: pasting a block is exactly the edit you most want
     * back, and it was exactly the one that used to drop the history. */
    strcpy(DOC, "keep this line\nand this one\nand a third"); CUR = 0;
    g_ms += 5000; frame(NULL);
    frame(SALL COPY);
    frame(END);
    g_ms += 5000;
    frame(PASTE);
    CHECK(strlen(DOC) > 60, "a whole document pasted onto the end");
    frame(UNDO);
    CHECK(!strcmp(DOC, "keep this line\nand this one\nand a third"),
          "and ONE undo takes the whole paste back");

    /* Select-all then a key, the destructive case. */
    strcpy(DOC, "precious\ntext"); CUR = 0; g_ms += 5000; frame(NULL);
    frame(SALL);
    g_ms += 5000;
    frame("x");
    CHECK(!strcmp(DOC, "x"), "one key replaced the document");
    frame(UNDO);
    CHECK(!strcmp(DOC, "precious\ntext"), "undo brings the document back");

    /* A command byte must never reach the text -- 0xF6/0xF7 are above 0x80,
     * which is where real characters live. */
    strcpy(DOC, "z"); CUR = 1; g_ms += 5000; frame(NULL);
    frame(UNDO REDO UNDO REDO);
    CHECK(!strcmp(DOC, "z"), "undo and redo bytes are never inserted as text");

    /* --- a bound value is only written when the element is flushed --- */
    float sx = seg_x_for_change();
    CHECK(sx >= 0, "a press on a DSL Segmented moves the value it was bound to");
    if (sx >= 0) {
        /* With Sync(): the app sees the change and persists it. */
        seg_sync = 1; seg_cfg = 0; seg_applied = 0;
        seg_frame(); seg_frame();
        seg_press(sx, 12);
        CHECK(seg_applied == 1 && seg_cfg != 0,
              "with Sync() the app sees the new value and persists it");

        /* Without it: the widget still moves, the app never learns, and the
         * setting is unchanged -- exactly what Settings' Keyboard pane did. */
        seg_sync = 0; seg_cfg = 0; seg_applied = 0;
        seg_frame(); seg_frame();
        seg_press(sx, 12);
        CHECK(seg_applied == 0 && seg_cfg == 0,
              "without it the change is read a flush too early and thrown away");
    }

    char sized_field[16]="value";
    ui_frame_begin();VStack(.key="sized-field") {TextField(sized_field,sizeof sized_field,"Value").width(112);}
    em_flush();ui_frame_end();ui_run_layout(400,100);
    float fx,fy,fw,field_h;
    CHECK(ui_rect_of(ui_first_child(ui_first_child(ui_root())),&fx,&fy,&fw,&field_h) && fw==112,
          "a text field honors its authored width");

    char query[32]="";bool submitted=false;
    /* A search field forwards its own submit edge, exactly once. */
    for(int pass=0;pass<3;pass++) {
        if(pass==2){ui_input_char('o');ui_input_char('k');ui_input_char('\n');}
        ui_frame_begin();VStack(.key="search-submit") {
            em_field_autofocus();submitted=SearchField(query,sizeof query,"Search").submitted();
        }
        em_flush();ui_frame_end();ui_run_layout(400,100);
    }
    CHECK(!strcmp(query,"ok") && submitted,"SearchField forwards Return to its submitted binding");

    under_clicked=0;under_frame();under_frame();
    ui_pointer(20,12,true);under_frame();ui_pointer(20,12,false);under_frame();
    CHECK(under_clicked==1,"a decorative overlay passes pointer clicks to its underlying control");

    em_set_viewport(400,300);chrome_frame();chrome_frame();
    em_feed_pointer(84,18,0,0,0,1);chrome_frame();g_ms+=700;chrome_frame();chrome_frame();
    em_feed_pointer(20,18,1,0,0,1);chrome_frame();
    CHECK(chrome_closed,"leaving the layout board for Close does not swallow the close press");
    em_feed_pointer(20,18,0,0,0,1);chrome_frame();

    /* A WINDOW DRAGGED INTO A CLAMP COMES BACK WITH THE HAND. The drag has to
     * take the window's place from the mover, or every pixel the clamp
     * refused is added to the next step and the window stays pinned while
     * the pointer goes home. The pointer an app sees is relative to where the
     * window really is, so the test hands it that. */
    em_window_set_mover(clamp_mover); em_window_bind(7,100,40);
    mv_x=100; mv_y=40;
    int gx=100+200, gy=40+18;
    em_feed_pointer((float)(gx-mv_x),(float)(gy-mv_y),0,0,0,1);chrome_frame();
    em_feed_pointer((float)(gx-mv_x),(float)(gy-mv_y),1,0,0,1);chrome_frame();
    for(int k=0;k<8;k++){gy-=5;em_feed_pointer((float)(gx-mv_x),(float)(gy-mv_y),1,0,0,1);chrome_frame();}
    int pinned=mv_y;
    for(int k=0;k<8;k++){gy+=5;em_feed_pointer((float)(gx-mv_x),(float)(gy-mv_y),1,0,0,1);chrome_frame();}
    em_feed_pointer((float)(gx-mv_x),(float)(gy-mv_y),0,0,0,1);chrome_frame();
    CHECK(pinned==20 && mv_x==100 && mv_y==40,"a window dragged against the bar comes back with the pointer");

    em_res_set_loader(icon_asset);
    uint32_t iw=0,ih=0;
    CHECK(em_image_at("/system/images/ui-close.eic",16,&iw,&ih)!=NULL,
          "window control regression uses the shipped EIC icon");
    em_feed_pointer(300,200,0,0,0,1);chrome_frame();chrome_frame();
    /* No intermediate hovered frame: pointer movement and press arrive in one
     * compositor sample. The old tint-derived key destroyed the capture. */
    em_feed_pointer(20,18,1,0,0,1);chrome_frame();
    CHECK(chrome_closed,"a close press survives the icon's simultaneous hover tint change");
    em_feed_pointer(20,18,0,0,0,1);chrome_frame();

    /* Switching icon families keeps colour tiles and original line assets in
     * the same process cache. Exercise every shipped icon, not one fallback. */
    DIR *images=opendir("system/images");unsigned icons=0,loaded=0;
    CHECK(images!=NULL,"shipped icon directory is available");
    if(images) {
        struct dirent *entry;
        while((entry=readdir(images))) {
            size_t n=strlen(entry->d_name);if(n<4 || strcmp(entry->d_name+n-4,".eic"))continue;
            char path[512];snprintf(path,sizeof path,"/system/images/%s",entry->d_name);
            icons++;if(em_image_at(path,54,&iw,&ih))loaded++;
        }
        closedir(images);
    }
    CHECK(icons>24 && loaded==icons,"colour and monochrome families coexist without dropping an icon");

    ui_frame_begin();em_new_frame();Overlay() {Dialog(.width=300) {Text("A readable decision");}}
    em_flush();ui_frame_end();ui_run_layout(400,300);
    struct instance_handle overlay=ui_first_child(ui_root());
    struct scene_node *dialog=scene_resolve(&SA,ui_scene_of(ui_first_child(overlay)));
    CHECK(dialog && dialog->data.rect.fill.kind==PAINT_SOLID && dialog->data.rect.fill.solid.a==1,
          "a default Dialog retains an opaque readable surface");

    /* a clear Window background: the theme's on an opaque window, clear on
     * one that shows through (the Terminal at 60% was the theme's colour) */
    for (int clear=0;clear<2;clear++) {
        em_window_set_clear(clear);
        ui_frame_begin();em_new_frame();Window("Clear",.background=(Color){0,0,0,0}) {Spacer();}
        em_flush();ui_frame_end();ui_run_layout(400,300);
        struct scene_node *win=scene_resolve(&SA,ui_scene_of(ui_first_child(ui_root())));
        bool painted=win && win->data.rect.fill.kind==PAINT_SOLID && win->data.rect.fill.solid.a>0;
        CHECK(clear?!painted:painted && win->data.rect.fill.solid.a==1,
              clear?"a window that shows through keeps a clear background clear"
                   :"an opaque window's clear background is the theme's");
    }
    em_window_set_clear(0);

    struct ui_appearance calm={.density=1,.text_scale=100,.radius=8,.reduce_motion=1};
    ui_theme_configure(&calm);em_set_clock(fake_clock);g_ms+=10000;em_toast("Ready",Success);
    ui_frame_begin();em_new_frame();VStack(.key="calm-toast") {em_toast_host();}
    em_flush();ui_frame_end();ui_run_layout(400,100);
    struct scene_node *toast=scene_resolve(&SA,ui_scene_of(ui_first_child(ui_first_child(ui_root()))));
    CHECK(toast && toast->opacity==1,"reduced motion presents a toast without a fade or slide");

    /* A glass box rebuilt with nothing changed stays clean. Its fill and border
     * are the material's; written first as the box's own and then as the
     * material's, they differed from the last frame every frame, and the dock
     * re-presented itself ten times a second doing nothing. The renderer clears
     * the flags after a frame; here the test does. */
    struct scene_node *glass=0;bool glass_clean=true,glass_tinted=false;
    for (int f=0;f<3;f++) {
        ui_frame_begin();em_new_frame();
        VStack(.key="glass-host") {HStack(.key="glass-dock",.height=40,.glass=1,.corner=14,.border=1,.shadow=2) {Spacer();}}
        em_flush();ui_frame_end();ui_run_layout(400,100);
        glass=scene_resolve(&SA,ui_scene_of(ui_first_child(ui_first_child(ui_root()))));
        if (!glass) break;
        if (f>0 && (glass->dirty || glass->dirty_content)) glass_clean=false;
        glass_tinted=glass->data.rect.fill.kind==PAINT_SOLID && glass->data.rect.fill.solid.a==ui_theme()->material_alpha &&
                     glass->border_width==1 && glass->border_color.a==ui_theme()->border_strong.a;
        glass->dirty=glass->dirty_content=false;
    }
    CHECK(glass && glass_clean,"a glass box rebuilt with nothing changed is not dirty");
    CHECK(glass_tinted,"its fill and border are still the material's");

    /* An idle menu-bar button rebuilt is not dirty either: it was painted the
     * hover colour and then cleared on every frame, and the bar re-presented
     * its whole strip on every tick. */
    struct scene_node *mb=0;bool menu_clean=true;
    ui_pointer(-100.0f,-100.0f,false);
    for (int f=0;f<3;f++) {
        ui_frame_begin();em_new_frame();
        HStack(.key="menu-host") {Menu("Idle") {MenuItem("Item");}}
        em_flush();ui_frame_end();ui_run_layout(400,100);
        mb=scene_resolve(&SA,ui_scene_of(ui_first_child(ui_first_child(ui_root()))));
        if (!mb) break;
        if (f>0 && (mb->dirty || mb->dirty_content)) menu_clean=false;
        mb->dirty=mb->dirty_content=false;
    }
    CHECK(mb && menu_clean && mb->data.rect.fill.solid.a==0,"an idle menu button rebuilt is clear and not dirty");

    navigation_regressions();

    /* em_spring: an entrance from `from`, a turn mid-way, the id copied */
    {
        struct ui_appearance moving={.density=1,.text_scale=100,.radius=8};
        ui_theme_configure(&moving);em_set_clock(fake_clock);g_ms+=10000;
        char id[16];snprintf(id,sizeof id,"enter%d",7);
        float v0=em_spring(id,0.0f,1.0f,EmSmooth);
        snprintf(id,sizeof id,"scribbled");                 /* the caller's buffer reused */
        g_ms+=60;float v1=em_spring("enter7",0.0f,1.0f,EmSmooth);
        CHECK(v0==0.0f && v1>0.05f && v1<0.9f,"a new spring starts at `from` and is on its way 60 ms later");
        CHECK(em_spring_moving("enter7"),"and says it is moving");
        g_ms+=40;float v2=em_spring("enter7",0.0f,0.0f,EmSmooth);
        CHECK(v2>v1,"turned round, it keeps the way it was going for a moment");
        for(int i=0;i<200 && em_spring_moving("enter7");i++){g_ms+=8;v2=em_spring("enter7",0.0f,0.0f,EmSmooth);}
        CHECK(v2==0.0f && !em_spring_moving("enter7"),"and comes to rest exactly on the new target");
        struct ui_appearance still={.density=1,.text_scale=100,.radius=8,.reduce_motion=1};
        ui_theme_configure(&still);
        CHECK(em_spring("enter8",0.0f,1.0f,EmSmooth)==1.0f,"with reduced motion a spring is its target");
    }

    /* em_spring_throw: a gesture hands a spring its place and its speed */
    {
        struct ui_appearance moving={.density=1,.text_scale=100,.radius=8};
        ui_theme_configure(&moving);g_ms+=10000;
        em_spring_throw("thrown",100.0f,0.0f);
        CHECK(em_spring("thrown",0.0f,100.0f,EmSmooth)==100.0f && !em_spring_moving("thrown"),
              "held by a hand, a spring is where the hand put it -- not at `from`");
        em_spring_throw("thrown",100.0f,3000.0f);         /* let go toward 400, fast */
        em_spring_throw("dropped",100.0f,0.0f);           /* ...and one let go still */
        (void)em_spring("thrown",0.0f,400.0f,EmSmooth);(void)em_spring("dropped",0.0f,400.0f,EmSmooth);
        g_ms+=60;
        float thrown=em_spring("thrown",0.0f,400.0f,EmSmooth),dropped=em_spring("dropped",0.0f,400.0f,EmSmooth);
        CHECK(thrown>dropped+40.0f,"thrown, it is well ahead of one let go still");   /* v0 t e^-wt: 61 px at best */
        em_spring_throw("back",300.0f,3000.0f);           /* thrown the wrong way: it turns */
        (void)em_spring("back",0.0f,0.0f,EmSmooth);
        g_ms+=30;float b1=em_spring("back",0.0f,0.0f,EmSmooth);
        for(int i=0;i<300 && em_spring_moving("back");i++){g_ms+=8;(void)em_spring("back",0.0f,0.0f,EmSmooth);}
        CHECK(b1>300.0f && em_spring("back",0.0f,0.0f,EmSmooth)==0.0f,"thrown away from home, it carries on, turns and comes to rest home");
        struct ui_appearance still={.density=1,.text_scale=100,.radius=8,.reduce_motion=1};
        ui_theme_configure(&still);
        em_spring_throw("still",50.0f,3000.0f);
        CHECK(em_spring("still",0.0f,400.0f,EmSmooth)==400.0f,"with reduced motion a throw lands at once");
    }

    printf("=== em-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
