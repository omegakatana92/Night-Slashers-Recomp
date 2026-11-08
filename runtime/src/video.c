/*
 * Night Slashers native runtime: DECO32 video (two DECO16IC tile generators, two
 * DECO 52 sprite generators, DECO 99 'ACE' palette and alpha mixer).
 *
 * Behaviour follows MAME (BSD-3-Clause, development reference only):
 *   src/mame/dataeast/fghthist_v.cpp   nslasher_state::screen_update_nslasher, mix_nslasher
 *   src/mame/dataeast/deco16ic.cpp     get_tile_info, scan_rows, custom_tilemap_draw
 *   src/mame/shared/decospr.cpp        draw_sprites_common (standard format, raw bitmap)
 *   src/mame/dataeast/deco_ace.cpp     palette_update, get_alpha
 *   src/mame/dataeast/fghthist.cpp     gfx layouts, nslasher bank/mix callbacks
 *
 * Lines are rendered on demand: a write to a tile generator's control registers first
 * renders every line the beam has already passed (MAME's update_partial), so raster
 * scroll effects land on the same lines as on the board.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "nsl/machine.h"
#include "nsl/cpu.h"
#include "nsl/rom.h"

#include <stdlib.h>
#include <string.h>

#define SCREEN_BOTTOM (NSL_SCREEN_TOP + NSL_SCREEN_HEIGHT)      /* 248, exclusive */

static uint8_t *g_tiles8;
static uint8_t *g_tiles16[2];
static uint8_t *g_sprites[2];

static uint32_t g_pens[4096];
static uint8_t g_pri[NSL_SCREEN_HEIGHT][NSL_SCREEN_WIDTH];
static uint16_t g_sprite_raw[2][NSL_SCREEN_HEIGHT][NSL_SCREEN_WIDTH];
static uint16_t g_alpha_tilemap[NSL_SCREEN_HEIGHT][NSL_SCREEN_WIDTH];

/* ---- graphics decode (MAME gfx_layout semantics: bit n = byte n/8, MSB first) --- */

static inline int layout_bit(const uint8_t *data, uint32_t offset) {
    return (data[offset >> 3] >> (7 - (offset & 7))) & 1;
}

/* charlayout / tilelayout: RGN_FRAC(1,2), planes { half+8, half, 8, 0 } */
static void decode_4bpp(const uint8_t *rom, uint32_t rom_bytes, int size, uint8_t *out, uint32_t count) {
    uint32_t half = rom_bytes * 4;                 /* RGN_FRAC(1,2) in bits */
    uint32_t planes[4] = { half + 8, half, 8, 0 };
    uint32_t tile_bits = (size == 8) ? 16 * 8 : 64 * 8;
    for (uint32_t code = 0; code < count; code++) {
        uint32_t base = code * tile_bits;
        uint8_t *dest = out + (size_t)code * size * size;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                uint32_t xo = (size == 8) ? (uint32_t)x : (x < 8 ? 16 * 8 * 2 + (uint32_t)x : (uint32_t)(x - 8));
                uint32_t offset = base + (uint32_t)y * 16 + xo;
                int pixel = 0;
                for (int p = 0; p < 4; p++) pixel = (pixel << 1) | layout_bit(rom, offset + planes[p]);
                dest[y * size + x] = (uint8_t)pixel;
            }
        }
    }
}

/* tilelayout_5bpp: RGN_FRAC(1,1), planes { 0, 8, 16, 24, 32 }, row stride 40 bits */
static void decode_5bpp(const uint8_t *rom, uint8_t *out, uint32_t count) {
    for (uint32_t code = 0; code < count; code++) {
        uint32_t base = code * 16 * 16 * 5;
        uint8_t *dest = out + (size_t)code * 256;
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                uint32_t xo = x < 8 ? 16 * 8 * 5 + (uint32_t)x : (uint32_t)(x - 8);
                uint32_t offset = base + (uint32_t)y * 40 + xo;
                int pixel = 0;
                for (int p = 0; p < 5; p++) pixel = (pixel << 1) | layout_bit(rom, offset + (uint32_t)p * 8);
                dest[y * 16 + x] = (uint8_t)pixel;
            }
        }
    }
}

void nsl_video_free_gfx(void) {
    free(g_tiles8);
    free(g_tiles16[0]);
    free(g_tiles16[1]);
    free(g_sprites[0]);
    free(g_sprites[1]);
    g_tiles8 = g_tiles16[0] = g_tiles16[1] = g_sprites[0] = g_sprites[1] = NULL;
    nsl_host.tiles8 = nsl_host.tiles16[0] = nsl_host.tiles16[1] = NULL;
    nsl_host.sprites[0] = nsl_host.sprites[1] = NULL;
}

int nsl_video_attach_gfx(const uint8_t *tiles1, const uint8_t *tiles2,
                         const uint8_t *sprites1, const uint8_t *sprites2) {
    nsl_video_free_gfx();
    nsl_host.tiles8_count = NSL_TILES_SIZE * 8 / 2 / (16 * 8);
    nsl_host.tiles16_count[0] = nsl_host.tiles16_count[1] = NSL_TILES_SIZE * 8 / 2 / (64 * 8);
    nsl_host.sprites_count[0] = NSL_SPRITES1_SIZE * 8 / (16 * 16 * 5);
    nsl_host.sprites_count[1] = NSL_SPRITES2_SIZE * 8 / 2 / (64 * 8);
    g_tiles8 = (uint8_t *)malloc((size_t)nsl_host.tiles8_count * 64);
    g_tiles16[0] = (uint8_t *)malloc((size_t)nsl_host.tiles16_count[0] * 256);
    g_tiles16[1] = (uint8_t *)malloc((size_t)nsl_host.tiles16_count[1] * 256);
    g_sprites[0] = (uint8_t *)malloc((size_t)nsl_host.sprites_count[0] * 256);
    g_sprites[1] = (uint8_t *)malloc((size_t)nsl_host.sprites_count[1] * 256);
    if (!g_tiles8 || !g_tiles16[0] || !g_tiles16[1] || !g_sprites[0] || !g_sprites[1]) {
        nsl_video_free_gfx();
        return -1;
    }
    decode_4bpp(tiles1, NSL_TILES_SIZE, 8, g_tiles8, nsl_host.tiles8_count);
    decode_4bpp(tiles1, NSL_TILES_SIZE, 16, g_tiles16[0], nsl_host.tiles16_count[0]);
    decode_4bpp(tiles2, NSL_TILES_SIZE, 16, g_tiles16[1], nsl_host.tiles16_count[1]);
    decode_5bpp(sprites1, g_sprites[0], nsl_host.sprites_count[0]);
    decode_4bpp(sprites2, NSL_SPRITES2_SIZE, 16, g_sprites[1], nsl_host.sprites_count[1]);
    nsl_host.tiles8 = g_tiles8;
    nsl_host.tiles16[0] = g_tiles16[0];
    nsl_host.tiles16[1] = g_tiles16[1];
    nsl_host.sprites[0] = g_sprites[0];
    nsl_host.sprites[1] = g_sprites[1];
    return 0;
}

/* ---- DECO ACE --------------------------------------------------------- */

static void palette_update(void) {
    const uint16_t *ace = nsl_machine.ace;
    int fadeptr = ace[0x20] & 0xff, fadeptg = ace[0x21] & 0xff, fadeptb = ace[0x22] & 0xff;
    int fadepsr = ace[0x23] & 0xff, fadepsg = ace[0x24] & 0xff, fadepsb = ace[0x25] & 0xff;
    uint16_t mode = ace[0x26];
    for (int i = 0; i < 2048; i++) {
        uint32_t entry = nsl_machine.palette_buffered[i];
        int b = (entry >> 16) & 0xff, g = (entry >> 8) & 0xff, r = entry & 0xff;
        g_pens[i + 2048] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        if (mode == 0x1000) {
            b = b + fadepsb > 0xff ? 0xff : b + fadepsb;
            g = g + fadepsg > 0xff ? 0xff : g + fadepsg;
            r = r + fadepsr > 0xff ? 0xff : r + fadepsr;
        } else {                                         /* 0x1100 and default */
            b = (uint8_t)(b + (((fadeptb - b) * fadepsb) / 255));
            g = (uint8_t)(g + (((fadeptg - g) * fadepsg) / 255));
            r = (uint8_t)(r + (((fadeptr - r) * fadepsr) / 255));
        }
        g_pens[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

static int ace_alpha(unsigned value) {
    value &= 0x1f;
    int alpha = nsl_machine.ace[value] & 0xff;
    if (alpha > 0x20) return 0x80;
    alpha = 255 - (alpha << 3);
    return alpha < 0 ? 0 : alpha;
}

static inline uint32_t alpha_blend(uint32_t d, uint32_t s, int level) {
    return ((((s & 0x0000ff) * (uint32_t)level + (d & 0x0000ff) * (uint32_t)(256 - level)) >> 8))
         | ((((s & 0x00ff00) * (uint32_t)level + (d & 0x00ff00) * (uint32_t)(256 - level)) >> 8) & 0x00ff00)
         | ((((s & 0xff0000) * (uint32_t)level + (d & 0xff0000) * (uint32_t)(256 - level)) >> 8) & 0xff0000);
}

/* ---- DECO16IC playfields ---------------------------------------------- */

typedef struct Playfield {
    const uint16_t *vram;
    const uint16_t *rowscroll;          /* low 16 bits of rowscroll RAM, 0x800 entries */
    uint16_t scrollx, scrolly;
    uint8_t control0, control1;
    uint32_t bank;
    uint32_t colour_bank;
    int is8x8;
    const uint8_t *gfx;
    uint32_t gfx_count;
    uint32_t colourbase;
} Playfield;

static uint16_t g_rowscroll16[4][0x800];

static void playfield_setup(Playfield *pf, unsigned chip, unsigned which) {
    const NslTilegen *gen = &nsl_machine.tilegen[chip];
    pf->vram = gen->vram[which];
    unsigned rs = chip * 2 + which;
    for (int i = 0; i < 0x800; i++) g_rowscroll16[rs][i] = (uint16_t)nsl_machine.rowscroll32[rs][i];
    pf->rowscroll = g_rowscroll16[rs];
    pf->scrollx = gen->control[which == 0 ? 1 : 3];
    pf->scrolly = gen->control[which == 0 ? 2 : 4];
    pf->control0 = (uint8_t)(which == 0 ? gen->control[5] : gen->control[5] >> 8);
    pf->control1 = (uint8_t)(which == 0 ? gen->control[6] : gen->control[6] >> 8);
    uint8_t bankval = (uint8_t)(which == 0 ? gen->control[7] : gen->control[7] >> 8);
    pf->bank = (uint32_t)(bankval & ~0xf) << 8;          /* nslasher_state::bank_callback */
    pf->colour_bank = gen->colour_bank[which];
    pf->is8x8 = (pf->control1 >> 7) & 1;
    if (pf->is8x8) {
        pf->gfx = nsl_host.tiles8;                       /* gfx 0: charlayout, colour base 0x800 */
        pf->gfx_count = nsl_host.tiles8_count;
        pf->colourbase = 0x800;
    } else {
        pf->gfx = nsl_host.tiles16[chip];                /* gfx 1 / gfx 2: tilelayout, base 0 */
        pf->gfx_count = nsl_host.tiles16_count[chip];
        pf->colourbase = 0;
    }
}

/* Pen index of the tilemap pixmap at source (sx, sy); 0 low nibble means transparent. */
static inline uint32_t playfield_pixel(const Playfield *pf, int is8x8, const uint8_t *gfx, uint32_t gfx_count,
                                       uint32_t colourbase, int sx, int sy, int *opaque) {
    unsigned index;
    int size, px, py;
    if (is8x8) {
        index = (unsigned)((sy >> 3) * 64 + (sx >> 3));
        size = 8; px = sx & 7; py = sy & 7;
    } else {
        int col = sx >> 4, row = sy >> 4;
        index = (unsigned)((col & 0x1f) + ((row & 0x1f) << 5) + ((col & 0x20) << 5) + ((row & 0x20) << 6));
        size = 16; px = sx & 15; py = sy & 15;
    }
    uint32_t tile = pf->vram[index & 0x7FF];
    uint32_t colour = (tile >> 12) & 0xf;
    if (tile & 0x8000) {
        if (pf->control1 & 1) { px = size - 1 - px; colour &= 7; }
        if (pf->control1 & 2) { py = size - 1 - py; colour &= 7; }
    }
    uint32_t code = ((tile & 0xfff) + pf->bank) % gfx_count;
    uint32_t pixel = gfx[(size_t)code * size * size + py * size + px];
    *opaque = pixel != 0;
    return colourbase + (((colour & 0x0f) + pf->colour_bank) * 16) + pixel;
}

/* deco16ic_device::custom_tilemap_draw for one or two (combined) playfields.
 * target_rgb != NULL: write pens; else target_ind != NULL: write pen indices. */
static void playfield_draw(const Playfield *pf, const Playfield *pf2, int y0, int y1,
                           uint32_t (*target_rgb)[NSL_SCREEN_WIDTH],
                           uint16_t (*target_ind)[NSL_SCREEN_WIDTH], uint8_t priority) {
    if (!(pf->control0 & 0x80)) return;                         /* playfield disabled */
    int is8x8 = pf->is8x8;
    if (pf2 && is8x8) return;                                    /* combine uses 16x16 only */
    const int width_mask = is8x8 ? 511 : 1023;
    const int height_mask = is8x8 ? 255 : 511;
    const int row_type = 1 << ((pf->control0 >> 3) & 0xf);
    const int col_type = 8 << (pf->control0 & 7);
    const int use_rows = (pf->control1 >> 6) & 1;
    const int use_cols = (pf->control1 >> 5) & 1;
    int src_y = (pf->scrolly + y0) & height_mask;
    for (int y = y0; y < y1; y++) {
        int src_x = pf->scrollx;
        if (use_rows) src_x += pf->rowscroll[(src_y / row_type) & 0x7FF];
        src_x &= width_mask;
        int row = y - NSL_SCREEN_TOP;
        for (int x = 0; x < NSL_SCREEN_WIDTH; x++) {
            int column_offset = use_cols ? pf->rowscroll[0x200 + ((src_x & 0x1ff) / col_type)] : 0;
            int sy = (src_y + column_offset) & height_mask;
            int opaque = 0;
            uint32_t p = playfield_pixel(pf, is8x8, pf->gfx, pf->gfx_count, pf->colourbase, src_x, sy, &opaque);
            if (pf2) {
                int opaque2 = 0;
                uint32_t p2 = playfield_pixel(pf2, 0, pf2->gfx, pf2->gfx_count, pf2->colourbase, src_x, sy, &opaque2);
                p = ((p & 0x70f) + (((p & 0x30) | (p2 & 0x0f)) << 4)) & 0x7ff;   /* nslasher mix_callback */
                opaque |= opaque2;
            }
            src_x = (src_x + 1) & width_mask;
            if (opaque) {
                if (target_rgb) target_rgb[row][x] = g_pens[p & 0xFFF];
                else target_ind[row][x] = (uint16_t)p;
                g_pri[row][x] |= priority;
            }
        }
        src_y = (src_y + 1) & height_mask;
    }
}

/* ---- DECO 52 sprites to raw bitmaps (decospr.cpp, standard format) ------ */

static void sprite_tile_raw(unsigned chip, uint32_t code, uint32_t colour_raw, int fx, int fy,
                            int sx, int sy, int y0, int y1) {
    const uint8_t *gfx = nsl_host.sprites[chip] + (size_t)(code % nsl_host.sprites_count[chip]) * 256;
    for (int ty = 0; ty < 16; ty++) {
        int y = sy + ty;
        if (y < y0 || y >= y1) continue;
        const uint8_t *src = gfx + (fy ? 15 - ty : ty) * 16;
        uint16_t *dest = g_sprite_raw[chip][y - NSL_SCREEN_TOP];
        for (int tx = 0; tx < 16; tx++) {
            int x = sx + tx;
            if (x < 0 || x >= NSL_SCREEN_WIDTH) continue;
            uint8_t pen = src[fx ? 15 - tx : tx];
            if (pen != 0) dest[x] = (uint16_t)(pen + colour_raw);
        }
    }
}

static void draw_sprites(unsigned chip, int y0, int y1) {
    for (int y = y0; y < y1; y++) memset(g_sprite_raw[chip][y - NSL_SCREEN_TOP], 0, sizeof(g_sprite_raw[0][0]));
    const uint16_t *spriteram = nsl_machine.sprite_buffered[chip];
    for (int offs = 0; offs < 0x800; offs += 4) {
        int sprite = spriteram[offs + 1];
        int y = spriteram[offs];
        int flash = (y >> 12) & 1;
        int w = (y >> 11) & 1;
        if (flash && (nsl_machine.frame_count & 1)) continue;
        int x = spriteram[offs + 2];
        int colour = (x >> 9) & 0x7f;
        if (y & 0x8000) colour |= 0x80;
        int fx = (y >> 13) & 1;
        int fy = (y >> 14) & 1;
        int tempwidth = (((y >> 10) & 1) << 1) | ((y >> 9) & 1);
        int multi = (1 << tempwidth) - 1;
        x &= 0x1ff;
        y &= 0x1ff;
        if (x >= 320) x -= 512;
        if (y >= 256) y -= 512;
        y = 240 - y;
        x = 304 - x;
        sprite &= ~multi;
        int inc;
        if (fy) inc = -1;
        else { sprite += multi; inc = 1; }
        /* set_flip_screen(true): sprites are flipped relative to the tilemaps */
        y = 240 - y;
        fy = !fy;
        x = 304 - x;
        fx = !fx;
        int mult2 = multi + 1;
        while (multi >= 0) {
            int ypos = y + 16 * multi;
            if (ypos <= SCREEN_BOTTOM - 1 && ypos >= NSL_SCREEN_TOP - 16) {
                uint32_t code = (uint32_t)(sprite - multi * inc);
                sprite_tile_raw(chip, code, (uint32_t)colour << 8, fx, fy, x, ypos, y0, y1);
                if (w) sprite_tile_raw(chip, code - (uint32_t)mult2, (uint32_t)colour << 8, fx, fy, x + 16, ypos, y0, y1);
            }
            multi--;
        }
    }
}

/* ---- nslasher_state::mix_nslasher ------------------------------------- */

static void mix(int y0, int y1, int mix_alpha_tilemap) {
    const unsigned pri = nsl_machine.pri;
    const uint32_t *pal0 = &g_pens[nsl_machine.sprite_colourbase[0]];
    const uint32_t *pal1 = &g_pens[nsl_machine.sprite_colourbase[1]];
    const uint32_t *pal2 = &g_pens[0];
    for (int y = y0; y < y1; y++) {
        int row = y - NSL_SCREEN_TOP;
        const uint16_t *sprite0 = g_sprite_raw[0][row];
        const uint16_t *sprite1 = g_sprite_raw[1][row];
        const uint16_t *alpha_tilemap = g_alpha_tilemap[row];
        const uint8_t *tilemap_pri = g_pri[row];
        uint32_t *dest = nsl_host.framebuffer[row];
        for (int x = 0; x < NSL_SCREEN_WIDTH; x++) {
            const unsigned v0 = sprite0[x];
            const unsigned v1 = sprite1[x];
            const unsigned pri0 = (v0 & 0x6000) >> 13;
            const unsigned pri1 = (v1 & 0x6000) >> 13;
            const unsigned col0 = (((v0 & 0x1f00) >> 8) % 16) * 32;
            unsigned col1 = (v1 & 0x0f00) >> 8;
            const int alpha1 = (v1 & 0x8000) != 0;
            const int alpha2 = !(v1 & 0x1000);

            unsigned coloffs = ((pri & 4) == 0) ? 0x800 : 0;
            int sprite1_drawn = 0;
            if ((v0 & 0xff) != 0) {
                if ((pri0 & 3) == 0 || (pri0 & 3) == 1 || ((pri0 & 3) == 2 && mix_alpha_tilemap)) {
                    dest[x] = pal0[coloffs | ((v0 & 0xff) + col0)];
                    sprite1_drawn = 1;
                } else if ((pri0 & 3) == 2) {
                    if (tilemap_pri[x] < 4) { dest[x] = pal0[coloffs | ((v0 & 0xff) + col0)]; sprite1_drawn = 1; }
                } else {
                    if (tilemap_pri[x] < 2) { dest[x] = pal0[coloffs | ((v0 & 0xff) + col0)]; sprite1_drawn = 1; }
                }
            }

            coloffs = (((pri & 4) == 0) && sprite1_drawn) ? 0x800 : 0;
            int alpha = ((!alpha1) || alpha2)
                ? ace_alpha((col1 & 0x8) ? (0x4 + ((col1 & 0x3) / 2)) : ((col1 & 0x7) / 2)) : 0xff;
            col1 = (col1 % 16) * 16;
            if ((v1 & 0xff) != 0) {
                uint32_t pen1 = pal1[coloffs | ((v1 & 0xff) + col1)];
                if (alpha1) {
                    if (pri1 == 0 && ((v0 & 0xff) == 0 || ((pri0 & 3) != 0 && (pri0 & 3) != 1 && (pri0 & 3) != 2))) {
                        if ((pri & 1) == 0 || ((pri & 1) == 1 && tilemap_pri[x] < 4)
                            || ((pri & 1) == 1 && (mix_alpha_tilemap && ((alpha_tilemap[x] & 0xf) == 0))))
                            dest[x] = alpha_blend(dest[x], pen1, alpha);
                    } else if (pri1 == 1 && ((pri & 1) == 0 || tilemap_pri[x] < 4)
                               && ((v0 & 0xff) == 0 || ((pri0 & 3) != 0 && (pri0 & 3) != 1
                                                       && ((pri & 1) == 0 || (pri0 & 3) != 2)))) {
                        dest[x] = alpha_blend(dest[x], pen1, alpha);
                    } else if (pri1 == 2 || pri1 == 3) {
                        dest[x] = alpha_blend(dest[x], pen1, alpha);
                    }
                } else {
                    if (pri1 == 0 && ((v0 & 0xff) == 0 || ((pri0 & 3) != 0)))
                        dest[x] = alpha_blend(dest[x], pen1, alpha);
                    else if (pri1 == 1 || pri1 == 2 || pri1 == 3)
                        dest[x] = alpha_blend(dest[x], pen1, alpha);
                }
            }

            if (mix_alpha_tilemap) {
                const unsigned p = alpha_tilemap[x];
                if (p & 0xf) {
                    if (((v0 & 0xff) == 0 || (pri0 & 3) == 2 || (pri0 & 3) == 3)
                        && ((v1 & 0xff) == 0 || (pri1 & 3) == 2 || (pri1 & 3) == 3 || alpha1)) {
                        int a = ace_alpha(0x17 + (((p & 0xf0) >> 4) / 2));
                        dest[x] = alpha_blend(dest[x], pal2[(coloffs | p) & 0xFFF], a);
                    }
                }
            }
        }
    }
}

/* ---- screen_update_nslasher over screen lines [y0, y1) ------------------ */

static void render_lines(int y0, int y1) {
    if (y0 < NSL_SCREEN_TOP) y0 = NSL_SCREEN_TOP;
    if (y1 > SCREEN_BOTTOM) y1 = SCREEN_BOTTOM;
    if (y0 >= y1 || !nsl_host.tiles8) return;

    palette_update();
    Playfield pf[2][2];
    for (unsigned chip = 0; chip < 2; chip++)
        for (unsigned which = 0; which < 2; which++) playfield_setup(&pf[chip][which], chip, which);

    const unsigned pri = nsl_machine.pri;
    int alpha_tilemap = (nsl_machine.ace[0x17] != 0) && (pri & 3);

    for (int y = y0; y < y1; y++) {
        int row = y - NSL_SCREEN_TOP;
        memset(g_pri[row], 0, sizeof(g_pri[row]));
        memset(g_alpha_tilemap[row], 0, sizeof(g_alpha_tilemap[row]));
        for (int x = 0; x < NSL_SCREEN_WIDTH; x++) nsl_host.framebuffer[row][x] = g_pens[0x300];
    }

    draw_sprites(0, y0, y1);
    draw_sprites(1, y0, y1);

    uint32_t (*fb)[NSL_SCREEN_WIDTH] = nsl_host.framebuffer;
    if (pri & 2) {
        playfield_draw(&pf[1][0], &pf[1][1], y0, y1, fb, NULL, 1);       /* tilemap_12_combine_draw */
        playfield_draw(&pf[0][1], NULL, y0, y1, fb, NULL, 4);
    } else {
        playfield_draw(&pf[1][1], NULL, y0, y1, fb, NULL, 1);
        if (pri & 1) {
            playfield_draw(&pf[0][1], NULL, y0, y1, fb, NULL, 2);
            if (alpha_tilemap) playfield_draw(&pf[1][0], NULL, y0, y1, NULL, g_alpha_tilemap, 4);
            else playfield_draw(&pf[1][0], NULL, y0, y1, fb, NULL, 4);
        } else {
            playfield_draw(&pf[1][0], NULL, y0, y1, fb, NULL, 2);
            if (alpha_tilemap) playfield_draw(&pf[0][1], NULL, y0, y1, NULL, g_alpha_tilemap, 4);
            else playfield_draw(&pf[0][1], NULL, y0, y1, fb, NULL, 4);
        }
    }

    mix(y0, y1, alpha_tilemap);
    playfield_draw(&pf[0][0], NULL, y0, y1, fb, NULL, 0);                 /* text layer on top */
}

/* ---- beam position and partial updates -------------------------------- */

int nsl_current_vpos(int *hpos) {
    uint64_t frame_vblank = nsl_cpu.next_irq_cycle - NSL_FRAME_CYCLES;   /* last VBLANK start */
    uint64_t t = nsl_cpu.cycles >= frame_vblank ? nsl_cpu.cycles - frame_vblank : 0;
    int line = (int)((248 + t / NSL_LINE_CYCLES) % 274);
    if (hpos) *hpos = (int)(t % NSL_LINE_CYCLES);
    return line;
}

void nsl_video_partial_update(void) {
    int hpos;
    int vpos = nsl_current_vpos(&hpos);
    if (vpos >= SCREEN_BOTTOM) return;                  /* VBLANK: belongs to the next frame */
    int ydelta = (hpos > NSL_SCREEN_WIDTH - 1) ? 0 : 1;
    int until = vpos - ydelta + 1;
    if (until > nsl_machine.rendered_until) {
        render_lines(nsl_machine.rendered_until, until);
        nsl_machine.rendered_until = until > SCREEN_BOTTOM ? SCREEN_BOTTOM : until;
    }
}

void nsl_video_end_frame(void) {
    render_lines(nsl_machine.rendered_until, SCREEN_BOTTOM);
    nsl_machine.rendered_until = NSL_SCREEN_TOP;
}

void nsl_video_begin_frame(void) {
    nsl_machine.rendered_until = NSL_SCREEN_TOP;
}
