/*
 * Night Slashers front end: drawing helpers (original UI, no game art).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_FE_UI_H
#define NSL_FE_UI_H

#include <SDL.h>

typedef struct UiColor { Uint8 r, g, b, a; } UiColor;

extern const UiColor ui_accents[8];
extern const char *ui_accent_names[8];

int ui_init(SDL_Renderer *renderer);
void ui_shutdown(void);
/* scale: pixel height of a glyph = 16 * scale / 2 (scale 2 = 16 px) */
int ui_text_width(const char *text, int scale);
int ui_glyph_count(const char *text);
int ui_text_wrapped(int x, int y, int width, const char *text, int scale, UiColor color);
void ui_text(int x, int y, const char *text, int scale, UiColor color);
void ui_text_shadow(int x, int y, const char *text, int scale, UiColor color);
void ui_text_center(int cx, int y, const char *text, int scale, UiColor color);
void ui_text_right(int rx, int y, const char *text, int scale, UiColor color);
void ui_fill(int x, int y, int w, int h, UiColor color);
void ui_frame(int x, int y, int w, int h, int thickness, UiColor color);
void ui_gradient(int x, int y, int w, int h, UiColor top, UiColor bottom);
UiColor ui_rgba(int r, int g, int b, int a);
UiColor ui_mix(UiColor a, UiColor b, float t);

#endif
