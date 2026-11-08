/*
 * Night Slashers front end: drawing helpers.
 *
 * Text uses the public-domain font8x8 glyphs, upscaled once with the EPX/Scale2x rule
 * to 16x16 so diagonals stay clean at Steam Deck and desktop sizes.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "ui.h"
#include "font8x8_basic.h"

#include <string.h>

const UiColor ui_accents[8] = {
    { 0xE0, 0x3A, 0x3A, 255 },   /* crimson */
    { 0xF2, 0x9C, 0x2B, 255 },   /* amber */
    { 0x3A, 0xB0, 0xE0, 255 },   /* steel blue */
    { 0x4C, 0xD1, 0x7A, 255 },   /* green */
    { 0xB0, 0x6C, 0xF0, 255 },   /* violet */
    { 0xF0, 0x5C, 0xA8, 255 },   /* magenta */
    { 0xE8, 0xE0, 0x50, 255 },   /* yellow */
    { 0xC8, 0xC8, 0xD0, 255 },   /* silver */
};
const char *ui_accent_names[8] = {
    "Crimson", "Amber", "Steel blue", "Green", "Violet", "Magenta", "Yellow", "Silver",
};

static SDL_Renderer *g_renderer;
static SDL_Texture *g_font;

#define GLYPH 16

static int glyph_bit(int c, int x, int y) {
    if (x < 0 || y < 0 || x >= 8 || y >= 8) return 0;
    return (font8x8_basic[c & 127][y] >> x) & 1;
}

int ui_init(SDL_Renderer *renderer) {
    g_renderer = renderer;
    /* 16 x 8 grid of 16x16 glyphs */
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, GLYPH * 16, GLYPH * 8, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return -1;
    SDL_FillRect(surface, NULL, 0);
    Uint32 *pixels = (Uint32 *)surface->pixels;
    int pitch = surface->pitch / 4;
    for (int c = 0; c < 128; c++) {
        int ox = (c % 16) * GLYPH, oy = (c / 16) * GLYPH;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int p = glyph_bit(c, x, y);
                int a = glyph_bit(c, x, y - 1), b = glyph_bit(c, x + 1, y);
                int cc = glyph_bit(c, x - 1, y), d = glyph_bit(c, x, y + 1);
                int e0 = p, e1 = p, e2 = p, e3 = p;           /* EPX */
                if (cc == a && cc != d && a != b) e0 = a;
                if (a == b && a != cc && b != d) e1 = b;
                if (d == cc && d != b && cc != a) e2 = cc;
                if (b == d && b != a && d != cc) e3 = d;
                Uint32 on = 0xFFFFFFFFu;
                pixels[(oy + y * 2) * pitch + ox + x * 2] = e0 ? on : 0;
                pixels[(oy + y * 2) * pitch + ox + x * 2 + 1] = e1 ? on : 0;
                pixels[(oy + y * 2 + 1) * pitch + ox + x * 2] = e2 ? on : 0;
                pixels[(oy + y * 2 + 1) * pitch + ox + x * 2 + 1] = e3 ? on : 0;
            }
        }
    }
    g_font = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (!g_font) return -1;
    SDL_SetTextureBlendMode(g_font, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g_font, SDL_ScaleModeLinear);
    return 0;
}

void ui_shutdown(void) {
    if (g_font) SDL_DestroyTexture(g_font);
    g_font = NULL;
}

static int advance(int scale) { return (GLYPH * scale / 2) * 7 / 8; }   /* slight tracking */

int ui_text_width(const char *text, int scale) {
    return (int)strlen(text) * advance(scale);
}

void ui_text(int x, int y, const char *text, int scale, UiColor color) {
    int size = GLYPH * scale / 2;
    SDL_SetTextureColorMod(g_font, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(g_font, color.a);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int c = *p < 128 ? *p : '?';
        SDL_Rect src = { (c % 16) * GLYPH, (c / 16) * GLYPH, GLYPH, GLYPH };
        SDL_Rect dst = { x, y, size, size };
        if (c != ' ') SDL_RenderCopy(g_renderer, g_font, &src, &dst);
        x += advance(scale);
    }
}

void ui_text_shadow(int x, int y, const char *text, int scale, UiColor color) {
    int offset = scale > 2 ? 2 : 1;
    ui_text(x + offset, y + offset, text, scale, ui_rgba(0, 0, 0, color.a * 3 / 4));
    ui_text(x, y, text, scale, color);
}

void ui_text_center(int cx, int y, const char *text, int scale, UiColor color) {
    ui_text_shadow(cx - ui_text_width(text, scale) / 2, y, text, scale, color);
}

void ui_text_right(int rx, int y, const char *text, int scale, UiColor color) {
    ui_text(rx - ui_text_width(text, scale), y, text, scale, color);
}

void ui_fill(int x, int y, int w, int h, UiColor color) {
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(g_renderer, &rect);
}

void ui_frame(int x, int y, int w, int h, int t, UiColor color) {
    ui_fill(x, y, w, t, color);
    ui_fill(x, y + h - t, w, t, color);
    ui_fill(x, y + t, t, h - 2 * t, color);
    ui_fill(x + w - t, y + t, t, h - 2 * t, color);
}

void ui_gradient(int x, int y, int w, int h, UiColor top, UiColor bottom) {
    int bands = h < 64 ? h : 64;
    if (bands <= 0) return;
    for (int i = 0; i < bands; i++) {
        int y0 = y + h * i / bands, y1 = y + h * (i + 1) / bands;
        ui_fill(x, y0, w, y1 - y0, ui_mix(top, bottom, bands > 1 ? (float)i / (bands - 1) : 0.0f));
    }
}

UiColor ui_rgba(int r, int g, int b, int a) {
    UiColor c = { (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a };
    return c;
}

UiColor ui_mix(UiColor a, UiColor b, float t) {
    UiColor c;
    c.r = (Uint8)(a.r + (b.r - a.r) * t);
    c.g = (Uint8)(a.g + (b.g - a.g) * t);
    c.b = (Uint8)(a.b + (b.b - a.b) * t);
    c.a = (Uint8)(a.a + (b.a - a.a) * t);
    return c;
}
