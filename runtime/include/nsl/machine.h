/*
 * Night Slashers native runtime: DECO32 board model.
 *
 * Memory map and device behaviour follow MAME 0.289's Data East driver
 * (src/mame/dataeast/fghthist.cpp, nslasher_map/nslasher_common_map) and its
 * DECO16IC, DECO ACE, DECO104 and EEPROM devices. MAME is a development reference
 * only; no MAME code is linked into the game.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_MACHINE_H
#define NSL_MACHINE_H

#include <stdint.h>

#define NSL_SCREEN_WIDTH 320
#define NSL_SCREEN_HEIGHT 240
#define NSL_PROGRAM_SIZE 0x100000
#define NSL_RAM_SIZE 0x20000

typedef struct NslInputs {
    /* active-low values as the board sees them */
    uint16_t in0;
    uint16_t in1;
} NslInputs;

typedef struct NslMachine {
    uint8_t *program;                 /* decrypted program image, owned by the caller */
    uint8_t ram[NSL_RAM_SIZE];

    uint8_t ace[0xA0];
    uint16_t palette[0x1000];
    uint16_t palette_buffer[0x1000];
    uint8_t spriteram[2][0x2000];
    uint8_t spritebuffer[2][0x2000];
    uint8_t tilevram[4][0x2000];      /* tilegen0 even/odd, tilegen1 even/odd */
    uint8_t rowscroll[4][0x2000];
    uint8_t tilecontrol[2][0x20];
    uint8_t color_bank[3];
    uint8_t eeprom[128];

    /* DECO104 protection / I/O chip */
    uint16_t deco_ram[2][0x80];
    uint8_t deco_region[6];
    uint8_t deco_bank;
    uint16_t deco_latchaddr;
    uint16_t deco_latchdata;
    uint8_t deco_latchflag;
    uint16_t deco_xor;
    uint16_t deco_nand;
    uint16_t soundlatch;
    uint32_t soundlatch_writes;

    uint16_t inputs_in0;
    uint16_t inputs_in1;
    uint32_t coin_counter;
    uint32_t volume;
    uint32_t vblank_ack_count;
    uint32_t palette_dma_count;
    uint32_t sprite_dma_count[2];
    uint32_t unmapped_reads;
    uint32_t unmapped_writes;

    uint8_t framebuffer[NSL_SCREEN_HEIGHT][NSL_SCREEN_WIDTH][3];
    uint32_t frame_count;
    uint32_t last_read_address;
    uint32_t last_write_address;
    int trace_bus;
    int vblank_active;
    uint64_t vblank_end_cycle;
    int trace_deco;
    int trace_eeprom;
} NslMachine;

extern NslMachine nsl_machine;

void nsl_machine_init(uint8_t *decrypted_program);
void nsl_machine_set_inputs(uint16_t in0, uint16_t in1);
void nsl_machine_set_vblank(int active);
int nsl_machine_load_eeprom(const char *path);
void nsl_machine_set_default_inputs(void);
void nsl_machine_render(void);

#endif
