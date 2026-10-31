/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-4: the stock project system is not used. These are the hooks the FM-1
 * SDK library calls; FM-4 keeps its own data (see fm4/fm4_core.c). */
#include "instrument.h"
#include <string.h>

/* The library's undo/clipboard borrows this buffer (4064 bytes). */
static struct groove_project scratch;
struct groove_project *groove_project_scratch(void) { return &scratch; }
void groove_project_init(void) { memset(&scratch, 0, sizeof scratch); }
void groove_project_poll(void) {}
void groove_project_metadata(const struct groove_project *p) { (void)p; }
void groove_project_name(char name[13]) { memcpy(name, "FM-4", 5); }
