/*
 * Night Slashers native runtime: game core API used by every front end.
 *
 * The core owns the loaded ROM regions, the translated CPU, the board and the sound
 * section. Front ends drive it one video frame at a time and never touch board
 * internals directly.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_CORE_H
#define NSL_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "nsl/machine.h"

#define NSL_FRAME_RATE (7000000.0 / (442.0 * 274.0))     /* 57.7959 Hz */

/* Inputs, active high for front ends; the core converts to the board's active-low
 * IN0/IN1 (fghthist.cpp INPUT_PORTS_START(nslasher)). */
enum {
    NSL_IN_UP = 1 << 0, NSL_IN_DOWN = 1 << 1, NSL_IN_LEFT = 1 << 2, NSL_IN_RIGHT = 1 << 3,
    NSL_IN_B1 = 1 << 4, NSL_IN_B2 = 1 << 5, NSL_IN_B3 = 1 << 6, NSL_IN_START = 1 << 7,
    NSL_IN_COIN = 1 << 8
};
enum { NSL_SYS_SERVICE_COIN = 1 << 0, NSL_SYS_TEST = 1 << 1 };

typedef struct NslInputState {
    uint16_t player[3];            /* NSL_IN_* per player; player 3 has no coin input */
    uint16_t system;               /* NSL_SYS_* */
} NslInputState;

extern const uint32_t nsl_translated_program_crc32;
extern const char nsl_translated_program_set[];

int nsl_core_load_rom(const char *path, char *error, size_t error_size);
int nsl_core_loaded(void);
void nsl_core_unload(void);
const char *nsl_core_rom_title(void);
uint32_t nsl_core_rom_fingerprint(void);     /* CRC32 over every ROM region the core uses */

void nsl_core_reset(void);                   /* board reset; EEPROM contents survive */
int nsl_core_run_frame(const NslInputState *input);   /* 0 ok, -1 CPU fault */
const char *nsl_core_fault_text(void);
uint64_t nsl_core_frame_number(void);

const uint32_t *nsl_core_framebuffer(void);  /* 320x240 0x00RRGGBB, valid after a frame */

/* Audio produced during the last frame, interleaved stereo int16 at
 * nsl_core_audio_rate(). Returns the number of stereo frames. */
int nsl_core_audio_rate(void);
size_t nsl_core_audio(const int16_t **samples);

/* EEPROM (the game's own settings and high scores) */
void nsl_core_get_eeprom(uint8_t out[128]);
void nsl_core_set_eeprom(const uint8_t in[128]);

/* Save states: see runtime/src/state.c for the format. */
size_t nsl_core_state_size(void);
int nsl_core_save_state(uint8_t *buffer, size_t size);
int nsl_core_load_state(const uint8_t *buffer, size_t size, char *error, size_t error_size);

/* Verified gadgets data (see docs/gadgets.md); returns 0 when unavailable. */
int nsl_core_png_encode(const uint32_t *pixels, int width, int height, uint8_t **png, size_t *png_size);

#endif
