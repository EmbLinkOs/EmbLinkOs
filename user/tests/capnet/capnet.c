/* capnet.c -- witness for the NETWORK and AUDIO gates, the two capability
 * classes that were gated in the kernel but had no test proving it.
 *
 * capgpu and capfs each cover one class in one direction at a time. This one
 * probes BOTH classes in a single run and encodes the answers as BITS, so one
 * binary and four spawns cover every combination -- including the case a
 * single-class witness cannot see: that holding one class does not
 * accidentally grant the other.
 *
 *   bit 0 (1)  NETWORK granted -- a socket was created
 *   bit 1 (2)  AUDIO   granted -- the device answered a query
 *
 *   exit 0 = neither, 1 = network only, 2 = audio only, 3 = both
 *   exit 8 = something OTHER than a clean denial happened, deliberately far
 *            from the four legal answers so a broken probe can never be
 *            mistaken for a passing one.
 *
 * The AUDIO probe uses the QUERY form (sample rate), not the claim form: a
 * witness must not take the speaker away from whatever else is running. The
 * gate is checked before the query is answered, so it proves the same thing.
 */
#include "embk.h"

int main(void) {
    int bits = 0;

    /* --- NETWORK: creating a socket is the install point that is gated. --- */
    int s = embk_net_socket(1);          /* 1 = stream */
    if (s >= 0) {
        bits |= 1;
        embk_close(s);
    } else if (s != -1) {
        return 8;                        /* not -EMBK_EPERM: an unexpected failure */
    }

    /* --- AUDIO: the query form, which is gated before it answers. --- */
    int rate = (int)embk_audio_rate();
    if (rate > 0)       bits |= 2;
    else if (rate != -1) return 8;       /* again: only a clean denial is legal */

    return bits;
}
