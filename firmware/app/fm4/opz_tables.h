/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM4_OPZ_TABLES_H
#define FM4_OPZ_TABLES_H
#include "fm4_port.h"
extern const uint16_t opz_sin_table[256];
extern const uint16_t opz_power_table[256];
extern const uint32_t opz_eg_increment[64];
extern const uint8_t opz_detune1[32][4];
extern const uint32_t opz_phase_step[768];
#endif
