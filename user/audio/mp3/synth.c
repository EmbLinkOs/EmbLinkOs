/* user/audio/mp3/synth.c -- see synth.h. */
#include <stdbool.h>
#include <math.h>
#include <string.h>

#include "synth.h"
#include "tables.h"

/* The 64x32 matrixing kernel: cos((16+i)(2k+1)pi/64).
 *
 * A formula, so it is computed rather than stored -- and computed ONCE, since
 * it is the same for every sample of every frame. This is the step that turns
 * 32 band values into the 64-point vector the window slides over. */
static float g_n[64][32];
static bool  g_ready;

static void prepare(void)
{
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 32; k++)
            g_n[i][k] = cosf((float)((16 + i) * (2 * k + 1)) *
                             (float)M_PI / 64.0f);
    g_ready = true;
}

void mp3_synth_granule(const float sb[32][18], float fifo[1024], float out[576])
{
    if (!g_ready) prepare();

    for (int t = 0; t < 18; t++) {
        /* Make room for this slot's 64 new values at the head of the history.
         * A rotating offset would avoid the copy, but it also has to be undone
         * in the U-extraction below, which is the fiddliest indexing in the
         * whole decoder -- this stays a straight shift on purpose. */
        memmove(fifo + 64, fifo, (1024 - 64) * sizeof fifo[0]);

        for (int i = 0; i < 64; i++) {
            float s = 0;
            for (int k = 0; k < 32; k++) s += g_n[i][k] * sb[k][t];
            fifo[i] = s;
        }

        /* Gather the 512 window inputs. The stride is the point: consecutive
         * blocks of 32 come from alternating halves of each 128-sample stretch
         * of history, which is how the polyphase decomposition lines the
         * overlapping filters back up. */
        float u[512];
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 32; j++) {
                u[i * 64 + j]      = fifo[i * 128 + j];
                u[i * 64 + 32 + j] = fifo[i * 128 + 96 + j];
            }

        /* Window, then fold the 512 down to 32 by summing every 32nd. */
        for (int j = 0; j < 32; j++) {
            float s = 0;
            for (int i = 0; i < 16; i++)
                s += u[j + 32 * i] * mp3_synth_window[j + 32 * i];
            out[t * 32 + j] = s;
        }
    }
}
