#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "drivers/audio/ac97.h"
#include "drivers/video/gpu.h"
#include "include/types.h"

/* Devices QEMU `virt` does not have -- docs/ARM64.md §2.6, "absent means
 * absent, not broken".
 *
 * READ THIS BEFORE ADDING TO IT.
 *
 * These are not placeholders for aarch64 versions of the same drivers. There
 * is no PS/2 controller on this machine, no AC'97 codec and no bochs VGA
 * aperture; those are ISA-era x86 devices and they are never going to appear.
 * Shared code calls them because on x86 they are how you reach a keyboard, a
 * speaker and a framebuffer, and the shared code is right to ask -- it is the
 * ANSWER that differs.
 *
 * What answers instead: virtio-input for the keyboard and mouse, virtio-snd
 * for audio, virtio-gpu for the display. Those are real work (docs/TODO.md),
 * and until they exist the honest reply to "is there a keyboard" is NO, not a
 * zero pretending to be a keystroke.
 *
 * Each function below therefore reports "nothing here" in whatever way its
 * caller already handles -- every one of these interfaces already has a
 * no-device case, because x86 machines without an AC'97 or without a mouse
 * exist too. Nothing here invents a behaviour; it selects one that already
 * had to work.
 *
 * WHEN A REAL DRIVER LANDS, DELETE ITS ENTRIES FROM THIS FILE. A leftover
 * definition here would silently win over the real one at link time. */

/* --- PS/2 keyboard: virtio-input replaces it ------------------------------ */
int  keyboard_event_pop(struct key_event *ev)          { (void)ev; return 0; }
uint8_t keyboard_mods(void)                            { return 0; }
char keyboard_getchar(void)                            { return 0; }
int  keyboard_has_char(void)                           { return 0; }
void keyboard_set_interrupt_target(uint32_t pid)       { (void)pid; }
void keyboard_inject_char(char c)                      { (void)c; }
void keyboard_set_grab(int grab, uint32_t pid)         { (void)grab; (void)pid; }
void keyboard_release_grab_pid(uint32_t pid)           { (void)pid; }

/* Returns "cancelled" rather than blocking: a caller that waits forever for a
 * keyboard that cannot exist is a hung process, and every caller of this
 * already handles the cancelled case. */
int  keyboard_getchar_blocking_cancelable(char *out)   { (void)out; return -1; }

/* --- PS/2 mouse ----------------------------------------------------------- */
void mouse_get_state(int32_t *x, int32_t *y, uint32_t *buttons) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (buttons) *buttons = 0;
}
int32_t mouse_take_wheel(void)                         { return 0; }

/* --- AC'97 audio: virtio-snd replaces it ---------------------------------- */
bool     ac97_present(void)                            { return false; }
uint32_t ac97_sample_rate(void)                        { return 0; }
uint32_t ac97_frames_per_buffer(void)                  { return 0; }
uint32_t ac97_fill(int i, const int16_t *f, uint32_t n) { (void)i; (void)f; (void)n; return 0; }
uint8_t  ac97_civ(void)                                { return 0; }
void     ac97_set_last(int last)                       { (void)last; }
void     ac97_play(int last)                           { (void)last; }
bool     ac97_done(int last)                           { (void)last; return true; }
void     ac97_stop(void)                               { }

/* --- bochs VGA: virtio-gpu is the display path here ----------------------- */
const struct gpu_driver *bochs_gpu_probe(void)         { return 0; }
