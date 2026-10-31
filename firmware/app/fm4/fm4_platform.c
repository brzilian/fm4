/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4 hardware services on the FM-1 SDK (see fm4_core.h). */
#include "fm4_core.h"
#include "fm1.h"

uint32_t fm4_hw_flash_size(void) { return fm1_flash_size(); }
int fm4_hw_flash_read(uint32_t off, void *data, size_t n) { return fm1_flash_read(off, data, n); }
int fm4_hw_flash_erase(uint32_t off) { return fm1_flash_erase(off); }
int fm4_hw_flash_write(uint32_t off, const void *data, size_t n) { return fm1_flash_write(off, data, n); }
int fm4_hw_midi_send(const uint8_t *msg, size_t n)
{
    /* USB and BLE outputs; a full queue (-2) is retried by the caller */
    unsigned ports = fm1_midi_ready_ports() & fm1_midi_output_ports();
    int result = 0;
    for (unsigned p = 0; p < 3; ++p) {
        if (!(ports & (1U << p))) continue;
        int r = fm1_midi_send_port(p, msg, n);
        if (r == -2) result = -2;
    }
    return result;
}
/* fm1_flash_write/erase already silence audio around the operation. */
void fm4_hw_audio_gate(int closed) { (void)closed; }
