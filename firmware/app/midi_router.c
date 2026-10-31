/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 MIDI input hook and the groove MIDI hooks the SDK library expects.
 * This file keeps its stock name so Build.ps1 compiles it without LTO:
 * fm1_app_midi_received overrides a weak default in the library. */
#include "instrument.h"
#include "fm4/fm4_core.h"

/* Groove (stock instrument) MIDI output hooks. The groove tracks are all
 * OFF under FM-4, so these stay silent; they must exist and live in SRAM. */
FM1_AUDIO_CODE unsigned groove_midi_mode(unsigned track) { (void)track; return 0; }
FM1_AUDIO_CODE void groove_midi_note(unsigned track, unsigned note, unsigned velocity)
{ (void)track; (void)note; (void)velocity; }
FM1_AUDIO_CODE void groove_midi_track_reset(unsigned track) { (void)track; }
FM1_AUDIO_CODE void groove_midi_transport(unsigned owner, uint8_t status) { (void)owner; (void)status; }
FM1_AUDIO_CODE void groove_midi_clock(unsigned owner, unsigned tempo) { (void)owner; (void)tempo; }
void groove_midi_init(void) {}
void groove_midi_poll(void) {}
void groove_midi_panic(void) {}
void groove_midi_clock_reset(void) {}

/* Routing task: complete short messages from DIN/TRS (0), USB (1), BLE (2).
 * Everything goes to the FM-4 audio ring; no blocking or Flash here. */
void fm1_app_midi_received(unsigned source, const uint8_t *m, size_t n)
{
    (void)source;
    if (!m || !n || n > 3 || m[0] < 0x80) return;
    if (m[0] == 0xF0 || m[0] == 0xF7) return;
    (void)fm4_post(m, (unsigned)n);
    fm1_control_request();      /* program changes etc. are finished on the control task */
}
