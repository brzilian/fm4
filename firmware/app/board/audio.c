/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm1_audio.h"
#include "fm1_gpio.h"
#include "fm1_engine_control.h"
#include "audio_output.h"
#include "fm4/fm4_core.h"
#include <string.h>

#define DMA_ALIGNED __attribute__((aligned(32)))
static int32_t receive[2][FM1_BLOCK_WORDS] DMA_ALIGNED;
static int32_t transmit[2][FM1_BLOCK_WORDS] DMA_ALIGNED;
static int32_t rendered[FM1_BLOCK_WORDS] DMA_ALIGNED;
static int32_t fm4_block[FM1_BLOCK_WORDS] DMA_ALIGNED;
static struct fm1_audio_stats statistics;
static uint32_t block_ticks, previous_tick;
static uint8_t initialized, previous_valid;
static volatile unsigned mute_depth;
/* FM-4 diagnostics: late-but-rendered blocks, FM-4 render peak (ticks). */
static uint32_t late_blocks, fm4_peak;

static inline __attribute__((always_inline)) uint32_t elapsed(uint32_t start, uint32_t end)
{
    return end >= start ? end - start : end + FM1_AUDIO_TIMER_PERIOD + 1U - start;
}
/* No libc calls in the DMA commit window, including during Flash writes. */
static FM1_AUDIO_CODE void fm1_audio_copy(volatile int32_t *out, const int32_t *in)
{
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) out[i] = in[i];
    __asm__ volatile("csync" ::: "memory");
}
FM1_AUDIO_CODE __attribute__((interrupt(""))) void fm1_audio_irq(void)
{
    uint8_t pending = fm1_audio_pending();
    unsigned rx = pending & 1U;
    uint32_t start = FM1_AUDIO_TIMER_COUNT, interval, used, late;
    unsigned half, committed = 0;
    fm1_audio_ack(pending);
    if (!rx || mute_depth) return;
    if (!previous_valid) { previous_tick = start; previous_valid = 1; return; }
    interval = elapsed(previous_tick, start);
    if (interval < 80000U) { ++statistics.short_intervals; return; }
    /* FM-4: an interrupt that starts late (another interrupt or a masked
     * section held it off) still has most of a block before its half is
     * played. Render it with a shorter commit window instead of dropping it,
     * and keep measuring from the expected tick so the next, on-time block
     * is not rejected as "short". Only a missed whole block resynchronises. */
    late = 0;
    if (interval > block_ticks + (block_ticks >> 3)) {
        late = interval - block_ticks;
        if (late >= block_ticks) {               /* a whole block was missed */
            previous_tick = start; ++statistics.overruns; return;
        }
        previous_tick = start >= late ? start - late : start + FM1_AUDIO_TIMER_PERIOD + 1U - late;
        if (late >= block_ticks >> 1) { ++statistics.overruns; return; }  /* no time left */
        ++late_blocks;
    } else previous_tick = start;
    if (interval < block_ticks - (block_ticks >> 3)) { ++statistics.short_intervals; return; }
    half = fm1_audio_free_half();
    /* The library renders the instrument; APP mixes external audio. */
    if (fm1_engine_render(rendered, FM1_BLOCK_FRAMES) != 0) {
        ++statistics.overruns;
        return;
    }
    /* FM-4: the stock groove instrument is idle (all tracks OFF) but still
     * rendered, which keeps the SDK's audio code linked and its timing
     * statistics valid. FM-4's TQ5 engine is added on top. */
    uint32_t fm4_start = FM1_AUDIO_TIMER_COUNT;
    fm4_core_render(fm4_block);
    uint32_t fm4_used = elapsed(fm4_start, FM1_AUDIO_TIMER_COUNT);
    if (fm4_used > fm4_peak) fm4_peak = fm4_used;
    multi_governor(fm4_used, block_ticks);
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) rendered[i] += fm4_block[i];
    fm1_output_mix(rendered);
    uint32_t flags = fm1_audio_irq_save();
    if (fm1_audio_half_owned(half) &&
        elapsed(start, FM1_AUDIO_TIMER_COUNT) + late < block_ticks - (block_ticks >> 3)) {
        fm1_audio_copy(transmit[half], rendered);
        committed = fm1_audio_half_owned(half);
    }
    fm1_audio_irq_restore(flags);
    if (!committed) ++statistics.overruns;
    pending = fm1_audio_pending();
    if (pending) {
        fm1_audio_ack(pending);
        if (pending & 1U) ++statistics.overruns;
    }
    used = elapsed(start, FM1_AUDIO_TIMER_COUNT);
    if (used > statistics.peak_ticks) statistics.peak_ticks = used;
    ++statistics.blocks;
}
void fm1_audio_suspend(void)
{
    uint32_t flags = fm1_device_irq_save();
    if (mute_depth++ == 0) {
        for (unsigned half = 0; half < 2; ++half)
            for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) transmit[half][i] = 0;
        fm1_output_reset();
        fm1_device_fence();
    }
    fm1_device_irq_restore(flags);
}
void fm1_audio_resume(void)
{
    uint32_t flags = fm1_device_irq_save();
    if (mute_depth && --mute_depth == 0) previous_valid = 0;
    fm1_device_irq_restore(flags);
}
int fm1_audio_start(void)
{
    if (initialized) return 0;
    uint32_t clock = fm1_device_clock();
    if (!clock) return -1;
    block_ticks = (uint32_t)((uint64_t)clock * FM1_BLOCK_FRAMES / FM1_AUDIO_HARDWARE_RATE);
    if (!block_ticks) return -1;
    memset(receive, 0, sizeof receive);
    memset(transmit, 0, sizeof transmit);
    fm1_output_reset();
    FM1_ALNK_CON0 = 0; FM1_ALNK_CON1 = 0; FM1_ALNK_CON2 = 0; FM1_ALNK_CON3 = 0;
    FM1_AUDIO_PORT_ROUTE &= ~(3U << 6); /* fixed port C route */
    const uint32_t outputs = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 6);
    FM1_PR(FM1_PC, FM1_OUT) |= outputs;
    FM1_PR(FM1_PC, FM1_DIE) |= 1U << 3;
    FM1_PR(FM1_PC, FM1_DIR) = (FM1_PR(FM1_PC, FM1_DIR) & ~outputs) | (1U << 3);
    FM1_PR(FM1_PC, FM1_PD) &= ~(1U << 3);
    FM1_PR(FM1_PC, FM1_PU) |= 1U << 3;
    FM1_AUDIO_CLOCK_SEL &= ~(15U << 8); /* 11.2896 MHz, 256 fs */
    FM1_ALNK_CON3 = 3U | (4U << 5);
    FM1_ALNK_CON0 = (1U << 8) | (1U << 7);
    FM1_ALNK_LEN = FM1_BLOCK_WORDS;
    FM1_ALNK_ADR0 = (uint32_t)(uintptr_t)receive;
    FM1_ALNK_ADR3 = (uint32_t)(uintptr_t)transmit;
    FM1_ALNK_CON1 = 1U | (1U << 2) | (1U << 3) | (1U << 12) | (1U << 14);
    fm1_device_fence();
    fm1_device_irq_attach(FM1_AUDIO_IRQ, fm1_audio_irq, FM1_AUDIO_PRIORITY);
    FM1_ALNK_CON0 |= 1U << 11;
    initialized = 1;
    return 0;
}
/* FM-4: render-time budget for the CPU meter (timer ticks per block). */
uint32_t fm4_audio_block_ticks(void) { return block_ticks; }
void fm4_audio_stats_reset_peak(void)
{
    uint32_t flags = fm1_device_irq_save();
    statistics.peak_ticks = 0;
    fm4_peak = 0;
    fm1_device_irq_restore(flags);
}
/* FM-4 render peak since the last reset, and late blocks that were rescued. */
uint32_t fm4_audio_render_peak(void) { return fm4_peak; }
uint32_t fm4_audio_late_blocks(void) { return late_blocks; }
void fm1_audio_stats_read(struct fm1_audio_stats *out)
{
    if (!out) return;
    uint32_t flags = fm1_device_irq_save();
    *out = statistics;
    fm1_device_irq_restore(flags);
}
