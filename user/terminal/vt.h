#ifndef EMBLINK_VT_H
#define EMBLINK_VT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VT_COLS 220
#define VT_ROWS 60
#define VT_HISTORY 300
#define VT_DEFAULT 0xffffffffu
#define VT_BOLD 1u
#define VT_DIM 2u
#define VT_ITALIC 4u
#define VT_UNDERLINE 8u
#define VT_INVERSE 16u
#define VT_HIDDEN 32u
#define VT_STRIKE 64u

/* A wide glyph owns its next cell (width=0). Combining storage is bounded;
 * this is Unicode cell handling, not a full grapheme/shaping engine. */
struct vt_cell {
    uint32_t rune, combining[2], fg, bg;
    uint8_t width, flags;
};
struct vt_line { struct vt_cell cells[VT_COLS]; unsigned used; bool wrapped, wrap_pad; };
struct vt_cursor {
    int x,y; uint32_t fg,bg; uint8_t flags;
    bool wrap,origin;
};
struct vt_screen {
    struct vt_line lines[VT_ROWS];
    unsigned order[VT_ROWS];
    struct vt_cursor cursor,saved;
    int top,bottom;
    bool addressed;
};
typedef void (*vt_reply_fn)(void *context,const char *bytes,size_t length);
struct vt {
    struct vt_screen primary,alternate;
    struct vt_line history[VT_HISTORY];
    unsigned history_head,history_count;
    int cols,rows;
    bool alt,alt_saved,autowrap,cursor_visible,application_cursor,bracketed_paste,keypad_application;
    /* Key settings, not terminal state: a reset keeps them. */
    bool backspace_sends_bs,alt_sends_escape;
    uint8_t cursor_style;
    bool tabs[VT_COLS],graphics[2];
    unsigned charset;
    uint64_t revision,scrolls;
    /* Bounded streaming state, shared by every feed call. */
    unsigned state,nparam; int params[16]; bool private_csi,bad_csi;
    unsigned intermediate,string_kind,string_escape,string_length;
    char string[256],title[256],directory[256];
    unsigned prompt_serial; bool shell_ready;
    /* Shell integration (OSC 133): the prompt's first row, as scrolls+row so
     * it stays right while the screen scrolls; whether the shell edits its own
     * line (A carried edit=shell); the last command's status (D). OSC 133;R,
     * EmbLink's own, returns the cursor to that row's start: a shell redraws
     * from there, wherever a resize or anything else has moved things. */
    bool edit_shell; int64_t prompt_row; int last_status;
    /* DECSET 2048: report the size in-band, now and after every resize. */
    bool inband_resize; int pixel_width,pixel_height;
    uint32_t utf8,utf8_min; unsigned utf8_left;
    vt_reply_fn reply; void *reply_context;
};
void vt_init(struct vt *,int cols,int rows,vt_reply_fn,void *context);
void vt_reset(struct vt *);
void vt_feed(struct vt *,const void *,size_t);
bool vt_resize(struct vt *,int cols,int rows);
void vt_clear_history(struct vt *);
void vt_prompt_room(struct vt *,unsigned extra_rows);
/* Before a resize: when a shell that edits its own line is at its prompt,
 * erase from the prompt's first row down and leave the cursor there. The
 * shell redraws when the size report arrives, so the old prompt is never
 * reflowed into a duplicate. Nothing happens otherwise. */
bool vt_prompt_erase(struct vt *);
/* After that resize: the prompt starts where the cursor was left. */
void vt_prompt_here(struct vt *);
/* CSI 48;rows;cols;height;width t -- DECSET 2048's report. */
size_t vt_size_report(const struct vt *,char out[48]);
struct vt_screen *vt_active(struct vt *);
const struct vt_cell *vt_view_cell(const struct vt *,int row,int col,unsigned back);
size_t vt_line_text(const struct vt *,int row,unsigned back,char *dst,size_t capacity);
int vt_width(uint32_t rune);
unsigned vt_encode(uint32_t rune,char out[4]);
uint32_t vt_color(unsigned index);
/* Shared terminal key encoder (xterm's PC-style keys); modifier bits: Shift=1,
 * Alt=2, Ctrl=4. A value below 256 is a byte the keyboard produced -- Alt puts
 * ESC in front of it when alt_sends_escape is on. The rest are keys:
 *   cursor keys, Home, End   CSI/SS3 x (application cursor), CSI 1;m x modified
 *   Insert Delete PgUp PgDn  CSI n ~, CSI n;m ~
 *   F1-F4                    SS3 P..S, CSI 1;m P..S modified
 *   F5-F12                   CSI 15 17 18 19 20 21 23 24 ~ (;m)
 *   Backspace                DEL, or ^H with backspace_sends_bs; Ctrl gives the other
 *   keypad                   its character, or SS3 p..y n M k m j o in application mode */
enum vt_key { VT_KEY_UP=256,VT_KEY_DOWN,VT_KEY_RIGHT,VT_KEY_LEFT,VT_KEY_HOME,
    VT_KEY_END,VT_KEY_DELETE,VT_KEY_INSERT,VT_KEY_PAGEUP,VT_KEY_PAGEDOWN,VT_KEY_TAB,
    VT_KEY_F1,VT_KEY_F12=VT_KEY_F1+11,VT_KEY_BACKSPACE,VT_KEY_ENTER,VT_KEY_ESCAPE,VT_KEY_MENU,
    VT_KEY_KP_0,VT_KEY_KP_9=VT_KEY_KP_0+9,VT_KEY_KP_DOT,VT_KEY_KP_ENTER,VT_KEY_KP_PLUS,
    VT_KEY_KP_MINUS,VT_KEY_KP_STAR,VT_KEY_KP_SLASH };
size_t vt_key_sequence(const struct vt *,unsigned key,unsigned mods,char out[16]);
#endif
