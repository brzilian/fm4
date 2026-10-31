/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4: the stock DX7 voice library is not used (FM-4 keeps TQ5 voices).
 * The SDK library still asks for DX7 programs when it builds its (idle)
 * groove tracks, so return a silent init voice. */
#include "fm_patch.h"

void groove_fm_library_init(void) {}

FM1_DSP_CODE void groove_fm_program(uint8_t p[156], unsigned program)
{
    (void)program;
    /* DX7 INIT VOICE with every operator output level at zero. */
    for (unsigned op = 0; op < 6; ++op) {
        uint8_t *o = p + 21 * op;
        for (unsigned i = 0; i < 21; ++i) o[i] = 0;
        o[0] = o[1] = o[2] = o[3] = 99;      /* rates */
        o[4] = 99;                           /* L1 */
        o[18] = 1;                           /* coarse */
        o[20] = 7;                           /* detune centre */
    }
    for (unsigned i = 126; i < 156; ++i) p[i] = 0;
    p[134] = 0; p[137] = 35; p[143] = 24;    /* LFO speed, transpose */
    for (unsigned i = 0; i < 10; ++i) p[145 + i] = (uint8_t)"FM-4      "[i];
    p[155] = 0x3f;
}
