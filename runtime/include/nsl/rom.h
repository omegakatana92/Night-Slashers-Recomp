/*
 * Night Slashers native runtime: player ROM loading, verification and region building.
 *
 * Reads the player's nslasher.zip (or an extracted directory), checks every file
 * against MAME's CRC32 list (src/mame/dataeast/fghthist.cpp, ROM_START(nslasher)) and
 * builds the regions the board needs: decrypted program (DECO156), decrypted tiles
 * (DECO56/DECO74 after MAME's reorder_tilegfx), sprite ROMs in MAME's ROM_LOAD40
 * layout, the Z80 sound program and the two OKI sample ROMs. No ROM data is stored
 * in this repository or in release packages.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_ROM_H
#define NSL_ROM_H

#include <stddef.h>
#include <stdint.h>

#define NSL_TILES_SIZE    0x200000u
#define NSL_SPRITES1_SIZE 0x640000u
#define NSL_SPRITES2_SIZE 0x100000u
#define NSL_SOUND_SIZE    0x10000u
#define NSL_OKI_SIZE      0x80000u

typedef struct NslRomImages {
    char set_name[16];
    char title[96];
    uint32_t program_crc;          /* CRC32 of the decrypted 1 MiB program */
    uint8_t *program;              /* 0x100000, decrypted, little-endian words */
    uint8_t *tiles1;               /* 0x200000 */
    uint8_t *tiles2;               /* 0x200000 */
    uint8_t *sprites1;             /* 0x640000, 5bpp */
    uint8_t *sprites2;             /* 0x100000, 4bpp */
    uint8_t *sound;                /* 0x10000 Z80 program */
    uint8_t *oki1;                 /* 0x80000 */
    uint8_t *oki2;                 /* 0x80000 */
} NslRomImages;

/* Returns 0 on success. On failure writes a human-readable explanation (missing or
 * mismatched files, wrong set, unreadable archive) into error. */
int nsl_rom_load(const char *path, NslRomImages *images, char *error, size_t error_size);
void nsl_rom_free(NslRomImages *images);

/* Quick check used by the ROM picker: identifies the set without building regions. */
int nsl_rom_identify(const char *path, char *summary, size_t summary_size);

uint32_t nsl_crc32(const void *data, size_t size);

#endif
