/*
 * Night Slashers native runtime: DECO32 board model.
 *
 * Memory map and device behaviour follow MAME's Data East driver
 * (src/mame/dataeast/fghthist.cpp, nslasher_map/nslasher_common_map) and its
 * DECO16IC, DECO ACE, DECO 52 sprite, DECO104 and EEPROM devices. MAME is a
 * development reference only; no MAME code is linked into the game.
 *
 * Every device on this board sits on the ARM's 32-bit bus with only some byte lanes
 * wired. Storage below mirrors MAME's device storage (16-bit words for the tilemap
 * chips and sprite RAM, 32-bit words for palette and rowscroll RAM), so byte, half
 * and word accesses all go through one lane-masked path.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_MACHINE_H
#define NSL_MACHINE_H

#include <stdint.h>

#define NSL_SCREEN_WIDTH 320
#define NSL_SCREEN_HEIGHT 240
#define NSL_SCREEN_TOP 8               /* MAME visible area: lines 8..247 */
#define NSL_PROGRAM_SIZE 0x100000
#define NSL_RAM_SIZE 0x20000

typedef struct NslTilegen {                 /* one DECO16IC (two playfields) */
    uint16_t vram[2][0x800];
    uint16_t control[8];
    uint16_t colour_bank[2];
} NslTilegen;

/* Everything here is board state and is serialised by save states. Pointers and
 * host-only fields live in NslMachineHost below. */
typedef struct NslMachine {
    uint8_t ram[NSL_RAM_SIZE];

    NslTilegen tilegen[2];
    uint32_t rowscroll32[4][0x800];
    uint16_t ace[0x28];
    uint32_t palette[2048];
    uint32_t palette_buffered[2048];
    uint16_t spriteram[2][0x800];
    uint16_t sprite_buffered[2][0x800];
    uint16_t sprite_colourbase[2];
    uint8_t pri;

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
    uint8_t soundlatch_irq;

    uint16_t inputs_in0;
    uint16_t inputs_in1;
    uint32_t volume;
    uint32_t vblank_ack_count;
    uint32_t palette_dma_count;
    uint32_t sprite_dma_count[2];
    uint32_t unmapped_reads;
    uint32_t unmapped_writes;
    uint32_t frame_count;
    int vblank_active;
    uint64_t vblank_end_cycle;
    int rendered_until;                     /* next screen line to render (8..248) */
} NslMachine;

typedef struct NslMachineHost {
    const uint8_t *program;                 /* decrypted program image, owned by the caller */
    const uint8_t *tiles8;                  /* decoded 8x8 chars, 64 bytes each */
    const uint8_t *tiles16[2];              /* decoded 16x16 tiles (tiles1, tiles2) */
    const uint8_t *sprites[2];              /* decoded 16x16 sprites (5bpp, 4bpp) */
    uint32_t tiles8_count, tiles16_count[2], sprites_count[2];
    uint32_t framebuffer[NSL_SCREEN_HEIGHT][NSL_SCREEN_WIDTH];   /* 0x00RRGGBB */
    uint32_t last_read_address;
    uint32_t last_write_address;
    int trace_bus;
    int trace_deco;
    int trace_eeprom;
} NslMachineHost;

extern NslMachine nsl_machine;
extern NslMachineHost nsl_host;

void nsl_machine_init(const uint8_t *decrypted_program);
void nsl_machine_reset(void);
void nsl_machine_set_inputs(uint16_t in0, uint16_t in1);
void nsl_machine_set_vblank(int active);
int nsl_machine_load_eeprom(const char *path);
int nsl_machine_save_eeprom(const char *path);
void nsl_machine_get_eeprom(uint8_t out[128]);
void nsl_machine_set_eeprom(const uint8_t in[128]);
void nsl_machine_set_default_inputs(void);

/* EEPROM device state for save states */
typedef struct NslEepromState {
    int32_t cs, clk, di, state, bits, locked;
    uint32_t accum, address, shift;
    uint64_t ready_cycle;
    uint16_t data[64];
} NslEepromState;
void nsl_eeprom_get_state(NslEepromState *state);
void nsl_eeprom_set_state(const NslEepromState *state);

/* video (runtime/src/video.c) */
int nsl_video_attach_gfx(const uint8_t *tiles1, const uint8_t *tiles2,
                         const uint8_t *sprites1, const uint8_t *sprites2);
void nsl_video_free_gfx(void);
void nsl_video_partial_update(void);       /* called before a tile-chip control write */
void nsl_video_end_frame(void);            /* VBLANK start: finish the frame */
void nsl_video_begin_frame(void);
int nsl_current_vpos(int *hpos);

#endif
