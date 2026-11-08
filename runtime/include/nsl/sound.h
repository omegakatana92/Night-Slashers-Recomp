/*
 * Night Slashers native runtime: sound section (Z80, YM2151, two OKI M6295).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_SOUND_H
#define NSL_SOUND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nsl_sound_init(const uint8_t *z80_program, const uint8_t *oki1, const uint8_t *oki2);
void nsl_sound_reset(void);
void nsl_sound_shutdown(void);
void nsl_sound_begin_frame(void);
void nsl_sound_latch_write(uint8_t value);      /* DECO104 sound latch, time-stamped */
void nsl_sound_end_frame(void);
int nsl_sound_output_rate(void);
size_t nsl_sound_output(const int16_t **samples);

/* save-state support: raw device state blob */
size_t nsl_sound_state_size(void);
void nsl_sound_save(uint8_t *out);
void nsl_sound_load(const uint8_t *in);

#ifdef __cplusplus
}
#endif

#endif
