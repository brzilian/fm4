/* Host tests for FM-4 effects: stability and decay of every algorithm. */
#include "fx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static fx_unit fx;

int main(void)
{
    static const char *names[FX_ALGOS] = { "off", "reverb", "delay", "pingpong", "chorus", "dist", "dist+rev", "dist+dly" };
    for (unsigned a = 1; a < FX_ALGOS; ++a) {
        fx_init(&fx);
        fx_params p = { (uint8_t)a, 99, 99, 99 };
        fx_set(&fx, &p);
        int32_t send[FM4_BLOCK * 2], mix[FM4_BLOCK * 2];
        long peak = 0, tail = 0, energy_first = 0;
        for (int b = 0; b < 3000; ++b) {
            memset(send, 0, sizeof send); memset(mix, 0, sizeof mix);
            if (b < 20) for (int s = 0; s < FM4_BLOCK * 2; ++s) send[s] = ((s * 7919) % 2000 - 1000) << 13; /* noise burst */
            fx_render(&fx, send, mix);
            for (int s = 0; s < FM4_BLOCK * 2; ++s) {
                long v = labs((long)mix[s]);
                if (v > peak) peak = v;
                if (b < 200) energy_first += v >> 8;
                if (b >= 2900) tail += v >> 8;
            }
        }
        printf("%-9s peak %8ld  early %10ld  tail %8ld\n", names[a], peak, energy_first, tail);
        CHECK(peak < (1L << 24), "%s peak %ld", names[a], peak);
        CHECK(energy_first > 0, "%s silent", names[a]);
        CHECK(tail * 50 < energy_first, "%s does not decay", names[a]);
    }
    printf(failures ? "%d FAILURES\n" : "all fx tests passed\n", failures);
    return failures ? 1 : 0;
}
