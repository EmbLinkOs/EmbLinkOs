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

/* --- PS/2 keyboard and mouse: GONE FROM HERE, and that is the point -------
 * virtio-input landed (drivers/input/virtio_input.c), so the eleven keyboard
 * stubs and the two mouse stubs that used to sit here are deleted, exactly as
 * the warning above says to do. They now resolve to the REAL implementations
 * in drivers/input/keyboard.c and drivers/input/mouse.c, whose PS/2 halves are
 * behind `#if defined(__x86_64__)` and whose policy halves are shared.
 *
 * Deleting them was not optional once the real drivers linked: a definition
 * left here would silently WIN at link time, and the machine would have had a
 * working keyboard driver and no keyboard. */

/* --- AC'97 audio: GONE FROM HERE TOO ---------------------------------------
 * virtio-snd landed (drivers/audio/virtio_snd.c) and implements the same nine
 * functions, so the stubs are deleted exactly as the keyboard's and mouse's
 * were. Those nine names are not "the AC'97 driver's interface" -- they are
 * THE audio driver interface this kernel has, and audio.c is written against
 * them. */

/* --- bochs VGA: virtio-gpu is the display path here ----------------------- */
const struct gpu_driver *bochs_gpu_probe(void)         { return 0; }
