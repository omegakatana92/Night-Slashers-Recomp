/*
 * Night Slashers native runtime: sound section placeholder (silent).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/sound.h"

#include <string.h>

#define RATE 48000
static int16_t g_buffer[4096 * 2];
static size_t g_count;

void nsl_sound_init(const uint8_t *z80_program, const uint8_t *oki1, const uint8_t *oki2) {
    (void)z80_program; (void)oki1; (void)oki2;
}
void nsl_sound_reset(void) {}
void nsl_sound_shutdown(void) {}
void nsl_sound_begin_frame(void) {}
void nsl_sound_end_frame(void) { g_count = 830; memset(g_buffer, 0, g_count * 4); }
int nsl_sound_output_rate(void) { return RATE; }
size_t nsl_sound_output(const int16_t **samples) { *samples = g_buffer; return g_count; }
size_t nsl_sound_state_size(void) { return 0; }
void nsl_sound_save(uint8_t *out) { (void)out; }
void nsl_sound_load(const uint8_t *in) { (void)in; }
