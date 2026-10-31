/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4: portability layer shared by the FM-1 firmware, host tests and WASM. */
#ifndef FM4_PORT_H
#define FM4_PORT_H
#include <stdint.h>
#include <stddef.h>

#if defined(FM4_HOST) || defined(FM1_HOST_TEST) || defined(__wasm__)
#define FM4_AUDIO_CODE
#define FM4_AUDIO_DATA
#else
/* Everything the audio interrupt touches must live in internal SRAM: the
 * firmware keeps rendering while Flash is being programmed. */
#include "fm1.h"
#define FM4_AUDIO_CODE FM1_AUDIO_CODE
#define FM4_AUDIO_DATA FM1_AUDIO_DATA
#endif

#define FM4_RATE 44100
#define FM4_BLOCK 128

/* Audio-safe byte fill/copy: libc lives in Flash, and the volatile pointer
 * stops the compiler from turning these loops back into memset/memcpy. */
static inline void fm4_zero(void *d, size_t n) { volatile uint8_t *p = (volatile uint8_t *)d; while (n--) *p++ = 0; }
static inline void fm4_copy(void *d, const void *s, size_t n)
{ volatile uint8_t *p = (volatile uint8_t *)d; const uint8_t *q = (const uint8_t *)s; while (n--) *p++ = *q++; }

static inline int32_t fm4_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
#endif
