/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 SysEx hook. Keeps its stock name so Build.ps1 compiles it without
 * LTO: fm1_app_sysex overrides a weak default in the SDK library and must
 * remain a real definition (Build.ps1 Test-Firmware checks it).
 * Firmware-update frames are consumed by the SDK before this hook. */
#include "instrument.h"
#include "fm4/fm4_core.h"

/* Routing task: copy the frame into FM-4's mailbox and wake the control
 * task, which parses it (TQ5 SysEx and the FM-4 "F4" extension). */
void fm1_app_sysex(const uint8_t *m, size_t n)
{
    if (!m || n < 5 || n > 1024 || m[0] != 0xF0 || m[n - 1] != 0xF7) return;
    if (m[1] != 0x43 && !(m[1] == FM4X_ID0 && m[2] == FM4X_ID1 && m[3] == FM4X_ID2)) return;
    fm4_sysex_in(m, n);
    fm1_control_request();
}

void groove_transfer_init(void) {}
/* Control task, every poll: FM-4 service work (SysEx, replies, MIDI out). */
void groove_transfer_poll(void) { fm4_core_poll(); }
