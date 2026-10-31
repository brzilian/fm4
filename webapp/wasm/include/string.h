/* Minimal string.h for the freestanding WASM build (implemented in fm4_wasm.c). */
#ifndef FM4_WASM_STRING_H
#define FM4_WASM_STRING_H
#include <stddef.h>
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
#endif
