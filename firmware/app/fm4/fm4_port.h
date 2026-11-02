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

/* Helpers used from the audio interrupt must never become out-of-line copies:
 * the firmware links with LTO at -inline-threshold=5, so a plain "static
 * inline" may be emitted once in .text, which is Flash (XIP). */
#define FM4_INLINE static inline __attribute__((always_inline))

/* Audio-safe byte fill/copy: libc lives in Flash, and the volatile pointer
 * stops the compiler from turning these loops back into memset/memcpy. */
FM4_INLINE void fm4_zero(void *d, size_t n) { volatile uint8_t *p = (volatile uint8_t *)d; while (n--) *p++ = 0; }
FM4_INLINE void fm4_copy(void *d, const void *s, size_t n)
{ volatile uint8_t *p = (volatile uint8_t *)d; const uint8_t *q = (const uint8_t *)s; while (n--) *p++ = *q++; }
/* Word versions for audio buffers (same reason: a plain "x[i] = 0" or
 * "a[i] = b[i]" loop is recognised as memset/memcpy, i.e. a Flash call). */
FM4_INLINE void fm4_zero32(int32_t *d, size_t n) { volatile int32_t *p = d; while (n--) *p++ = 0; }
FM4_INLINE void fm4_copy32(int32_t *d, const int32_t *s, size_t n) { volatile int32_t *p = d; while (n--) *p++ = *s++; }

FM4_INLINE int32_t fm4_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
#endif
