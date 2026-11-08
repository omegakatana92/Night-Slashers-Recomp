/*
 * Night Slashers front end (SDL2): window, frame pacing, audio, input, save states,
 * rewind, recording, screenshots and EEPROM persistence. Menus live in menus.c.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _WIN32
#define SDL_MAIN_HANDLED          /* Windows uses SDL2main for the GUI-subsystem entry point */
#endif
#include <SDL.h>

#include "app.h"
#include "ui.h"
#include "nsl/core.h"
#include "nsl/interp.h"
#include "nsl/platform.h"
#include "nsl/state.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

App g_app;
extern int nsl_state_compression_level;

/* ---- logging and toasts --------------------------------------------------- */

void app_log(const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fprintf(stderr, "%s\n", line);
    FILE *file = g_app.log_path[0] ? nsl_fopen(g_app.log_path, "a") : NULL;
    if (file) {
        time_t now = time(NULL);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(file, "[%s] %s\n", stamp, line);
        fclose(file);
    }
}

uint64_t app_now_ms(void) { return SDL_GetTicks64(); }

void app_toast(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_app.toast, sizeof(g_app.toast), format, args);
    va_end(args);
    g_app.toast_until = app_now_ms() + 2200;
}

static void settings_path(char *out, size_t size) {
    nsl_path_join(out, size, g_app.data_dir, "settings.ini");
}

static void save_settings(void) {
    char path[1100];
    settings_path(path, sizeof(path));
    if (settings_save(&g_settings, path) != 0) app_log("cannot write %s", path);
}

/* ---- EEPROM (the game's own settings and high scores) ---------------------- */

static void eeprom_path(char *out, size_t size) {
    char dir[1100];
    nsl_path_join(dir, sizeof(dir), g_app.data_dir, "nvram");
    nsl_make_directories(dir);
    nsl_path_join(out, size, dir, "nslasher.eeprom");
}

void app_save_eeprom(void) {
    if (!nsl_core_loaded()) return;
    uint8_t now[128];
    nsl_core_get_eeprom(now);
    if (!memcmp(now, g_app.eeprom_saved, sizeof(now))) return;
    char path[1200];
    eeprom_path(path, sizeof(path));
    if (nsl_write_file_atomic(path, now, sizeof(now)) == 0) memcpy(g_app.eeprom_saved, now, sizeof(now));
    else app_log("cannot write EEPROM %s", path);
}

static void load_eeprom(void) {
    char path[1200];
    eeprom_path(path, sizeof(path));
    size_t size = 0;
    uint8_t *data = nsl_read_file(path, &size);
    if (data && size == 128) {
        nsl_core_set_eeprom(data);
        memcpy(g_app.eeprom_saved, data, 128);
    } else {
        nsl_core_get_eeprom(g_app.eeprom_saved);
    }
    free(data);
}

/* ---- ROM -------------------------------------------------------------------- */

int app_load_rom(const char *path) {
    char error[1024];
    if (nsl_core_loaded() && g_app.game_active) app_leave_game(1);
    if (nsl_core_load_rom(path, error, sizeof(error)) != 0) {
        snprintf(g_app.rom_status, sizeof(g_app.rom_status), "%s", error);
        g_app.rom_ok = 0;
        app_log("ROM %s rejected: %s", path, error);
        return -1;
    }
    snprintf(g_settings.rom_path, sizeof(g_settings.rom_path), "%s", path);
    snprintf(g_app.rom_status, sizeof(g_app.rom_status), "%s", nsl_core_rom_title());
    g_app.rom_ok = 1;
    load_eeprom();
    save_settings();
    app_log("ROM loaded: %s (%s)", path, nsl_core_rom_title());
    return 0;
}

/* ---- states ------------------------------------------------------------------- */

void app_state_path(int slot, char *out, size_t size) {
    char name[32];
    if (slot < 0) snprintf(name, sizeof(name), "auto.nss");
    else snprintf(name, sizeof(name), "slot%d.nss", slot + 1);
    nsl_path_join(out, size, g_app.state_dir, name);
}

int app_state_exists(int slot) {
    char path[1200];
    app_state_path(slot, path, sizeof(path));
    return nsl_path_exists(path);
}

int app_save_state(int slot) {
    if (!g_app.game_active) return -1;
    size_t capacity = nsl_core_state_size();
    uint8_t *buffer = (uint8_t *)malloc(capacity);
    if (!buffer) return -1;
    nsl_state_compression_level = 6;
    int size = nsl_core_save_state(buffer, capacity);
    if (size <= 0) {
        free(buffer);
        app_toast("Save failed");
        return -1;
    }
    nsl_state_set_timestamp(buffer, (size_t)size, (int64_t)time(NULL));
    char path[1200];
    app_state_path(slot, path, sizeof(path));
    int result = nsl_write_file_atomic(path, buffer, (size_t)size);
    free(buffer);
    if (result != 0) {
        app_toast("Cannot write the save state");
        app_log("cannot write state %s", path);
        return -1;
    }
    if (slot < 0) app_toast("Autosaved");
    else app_toast("Saved to slot %d", slot + 1);
    return 0;
}

static void release_all_inputs(void) {
    memset(&g_app.input, 0, sizeof(g_app.input));
    g_app.suppress_game_input = 1;
    g_app.back_held_alone = 0;
    memset(g_app.coin_pulse, 0, sizeof(g_app.coin_pulse));
    g_app.service_pulse = 0;
}

int app_load_state(int slot) {
    char path[1200], error[256];
    app_state_path(slot, path, sizeof(path));
    size_t size = 0;
    uint8_t *data = nsl_read_file(path, &size);
    if (!data) {
        app_toast(slot < 0 ? "No autosave yet" : "Slot %d is empty", slot + 1);
        return -1;
    }
    int result = nsl_core_load_state(data, size, error, sizeof(error));
    free(data);
    if (result != 0) {
        app_toast("Cannot load: %s", error);
        app_log("state %s rejected: %s", path, error);
        return -1;
    }
    release_all_inputs();
    if (g_app.audio) SDL_ClearQueuedAudio(g_app.audio);
    g_app.rewind_count = 0;
    g_app.next_frame = SDL_GetPerformanceCounter();
    if (slot < 0) app_toast("Resumed from autosave");
    else app_toast("Loaded slot %d", slot + 1);
    return 0;
}

int app_state_info(int slot, char *text, size_t size, uint32_t *preview) {
    char path[1200], error[160];
    app_state_path(slot, path, sizeof(path));
    size_t length = 0;
    uint8_t *data = nsl_read_file(path, &length);
    if (!data) {
        snprintf(text, size, "Empty");
        return -1;
    }
    NslStateHeader header;
    int ok = nsl_state_peek(data, length, &header, error, sizeof(error)) == 0;
    if (ok && nsl_state_check(data, length, error, sizeof(error)) != 0) ok = 0;
    if (ok) {
        time_t when = (time_t)header.timestamp;
        char stamp[40] = "unknown time";
        if (when > 0) strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", localtime(&when));
        snprintf(text, size, "%s", stamp);
    } else {
        snprintf(text, size, "Unusable: %s", error);
    }
    if (preview && nsl_state_preview(data, length, preview) != 0) memset(preview, 0, 320 * 240 * 4);
    free(data);
    return ok ? 0 : -2;
}

void app_screenshot(void) {
    if (!g_app.game_active) return;
    char dir[1100], name[96], path[1300];
    nsl_path_join(dir, sizeof(dir), g_app.data_dir, "screenshots");
    nsl_make_directories(dir);
    time_t now = time(NULL);
    strftime(name, sizeof(name), "nslasher_%Y%m%d_%H%M%S.png", localtime(&now));
    nsl_path_join(path, sizeof(path), dir, name);
    uint8_t *png = NULL;
    size_t size = 0;
    if (nsl_core_png_encode(nsl_core_framebuffer(), NSL_SCREEN_WIDTH, NSL_SCREEN_HEIGHT, &png, &size) == 0
        && nsl_write_file_atomic(path, png, size) == 0) {
        app_toast("Screenshot saved: %s", name);
    } else {
        app_toast("Screenshot failed");
    }
    free(png);
}

/* ---- rewind (bounded ring of compressed states) ---------------------------- */

void app_rewind_configure(void) {
    for (int i = 0; i < g_app.rewind_capacity; i++) free(g_app.rewind[i]);
    free(g_app.rewind);
    free(g_app.rewind_size);
    g_app.rewind = NULL;
    g_app.rewind_size = NULL;
    g_app.rewind_capacity = g_app.rewind_head = g_app.rewind_count = 0;
    if (g_settings.rewind_seconds <= 0) return;
    g_app.rewind_capacity = (int)(g_settings.rewind_seconds * NSL_FRAME_RATE / 2.0);
    g_app.rewind = (uint8_t **)calloc((size_t)g_app.rewind_capacity, sizeof(uint8_t *));
    g_app.rewind_size = (int *)calloc((size_t)g_app.rewind_capacity, sizeof(int));
}

static void rewind_push(void) {
    if (!g_app.rewind_capacity || (nsl_core_frame_number() & 1)) return;
    static uint8_t *scratch;
    static size_t scratch_size;
    size_t need = nsl_core_state_size();
    if (scratch_size < need) {
        free(scratch);
        scratch = (uint8_t *)malloc(need);
        scratch_size = scratch ? need : 0;
        if (!scratch) return;
    }
    nsl_state_compression_level = 1;
    int size = nsl_core_save_state(scratch, scratch_size);
    nsl_state_compression_level = 6;
    if (size <= 0) return;
    int index = g_app.rewind_head;
    uint8_t *copy = (uint8_t *)realloc(g_app.rewind[index], (size_t)size);
    if (!copy) return;
    memcpy(copy, scratch, (size_t)size);
    g_app.rewind[index] = copy;
    g_app.rewind_size[index] = size;
    g_app.rewind_head = (index + 1) % g_app.rewind_capacity;
    if (g_app.rewind_count < g_app.rewind_capacity) g_app.rewind_count++;
}

static int rewind_pop(void) {
    if (!g_app.rewind_count) return -1;
    int index = (g_app.rewind_head - 1 + g_app.rewind_capacity) % g_app.rewind_capacity;
    char error[128];
    int result = nsl_core_load_state(g_app.rewind[index], (size_t)g_app.rewind_size[index], error, sizeof(error));
    g_app.rewind_head = index;
    g_app.rewind_count--;
    return result;
}

/* ---- input recording (deterministic replay: start state + per-frame inputs) ---- */

static void recording_path(char *out, size_t size) {
    char dir[1100];
    nsl_path_join(dir, sizeof(dir), g_app.data_dir, "recordings");
    nsl_make_directories(dir);
    nsl_path_join(out, size, dir, "last.nsr");
}

void app_record_start(void) {
    if (!g_app.game_active || g_app.recording) return;
    size_t capacity = nsl_core_state_size();
    free(g_app.rec_state);
    g_app.rec_state = (uint8_t *)malloc(capacity);
    g_app.rec_state_size = g_app.rec_state ? nsl_core_save_state(g_app.rec_state, capacity) : -1;
    if (g_app.rec_state_size <= 0) { app_toast("Cannot start recording"); return; }
    g_app.rec_count = 0;
    g_app.recording = 1;
    app_toast("Recording inputs");
}

void app_record_stop(void) {
    if (!g_app.recording) return;
    g_app.recording = 0;
    char path[1200];
    recording_path(path, sizeof(path));
    size_t total = 16 + (size_t)g_app.rec_state_size + (size_t)g_app.rec_count * sizeof(NslInputState);
    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) return;
    memcpy(out, "NSLREC01", 8);
    uint32_t frames = (uint32_t)g_app.rec_count, state = (uint32_t)g_app.rec_state_size;
    memcpy(out + 8, &frames, 4);
    memcpy(out + 12, &state, 4);
    memcpy(out + 16, g_app.rec_state, state);
    memcpy(out + 16 + state, g_app.rec_inputs, (size_t)frames * sizeof(NslInputState));
    if (nsl_write_file_atomic(path, out, total) == 0) app_toast("Recording saved (%u frames)", frames);
    else app_toast("Cannot write the recording");
    free(out);
}

int app_playback_start(void) {
    char path[1200], error[160];
    recording_path(path, sizeof(path));
    size_t size = 0;
    uint8_t *data = nsl_read_file(path, &size);
    if (!data || size < 16 || memcmp(data, "NSLREC01", 8)) {
        free(data);
        app_toast("No recording found");
        return -1;
    }
    uint32_t frames, state;
    memcpy(&frames, data + 8, 4);
    memcpy(&state, data + 12, 4);
    if (16 + (size_t)state + (size_t)frames * sizeof(NslInputState) > size) {
        free(data);
        app_toast("The recording is damaged");
        return -1;
    }
    if (nsl_core_load_state(data + 16, state, error, sizeof(error)) != 0) {
        free(data);
        app_toast("Cannot play: %s", error);
        return -1;
    }
    free(g_app.rec_inputs);
    g_app.rec_inputs = (NslInputState *)malloc((size_t)frames * sizeof(NslInputState) + 1);
    memcpy(g_app.rec_inputs, data + 16 + state, (size_t)frames * sizeof(NslInputState));
    g_app.rec_count = (int)frames;
    g_app.rec_capacity = (int)frames;
    g_app.rec_position = 0;
    g_app.playing_back = 1;
    free(data);
    g_app.game_active = 1;
    g_app.mode = MODE_GAME;
    release_all_inputs();
    app_toast("Playing recording");
    return 0;
}

/* ---- session --------------------------------------------------------------- */

void app_start_game(void) {
    if (!nsl_core_loaded()) return;
    nsl_core_reset();
    load_eeprom();
    g_app.game_active = 1;
    g_app.mode = MODE_GAME;
    g_app.paused = 0;
    g_app.rewind_count = 0;
    release_all_inputs();
    g_app.next_frame = SDL_GetPerformanceCounter();
}

void app_continue_game(void) {
    if (!nsl_core_loaded()) return;
    if (!g_app.game_active) {
        nsl_core_reset();
        g_app.game_active = 1;
    }
    if (app_load_state(-1) != 0) {
        g_app.game_active = 0;
        return;
    }
    g_app.mode = MODE_GAME;
}

void app_restart_game(void) {
    app_start_game();
    app_toast("Game restarted");
}

void app_leave_game(int to_menu) {
    if (g_app.recording) app_record_stop();
    g_app.playing_back = 0;
    if (g_app.game_active && g_settings.autosave) app_save_state(-1);
    app_save_eeprom();
    g_app.game_active = 0;
    if (to_menu) {
        g_app.mode = MODE_MENU;
        menus_open_main();
    }
}

void app_quit(void) {
    if (g_app.game_active) app_leave_game(0);
    g_app.running = 0;
}

/* ---- video ----------------------------------------------------------------- */

static void rebuild_overlays(int w, int h) {
    if (g_app.mask_texture && g_app.mask_w == w && g_app.mask_h == h && g_app.mask_kind == g_settings.mask_type
        && g_app.mask_strength == g_settings.mask_strength && g_app.scan_strength == g_settings.scanlines) return;
    if (g_app.mask_texture) SDL_DestroyTexture(g_app.mask_texture);
    if (g_app.scanline_texture) SDL_DestroyTexture(g_app.scanline_texture);
    g_app.mask_texture = g_app.scanline_texture = NULL;
    g_app.mask_w = w; g_app.mask_h = h;
    g_app.mask_kind = g_settings.mask_type;
    g_app.mask_strength = g_settings.mask_strength;
    g_app.scan_strength = g_settings.scanlines;
    if (w <= 0 || h <= 0) return;
    /* scanlines: 2 rows per game line, multiplied over the picture */
    if (g_settings.scanlines > 0) {
        g_app.scanline_texture = SDL_CreateTexture(g_app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1, 480);
        if (g_app.scanline_texture) {
            Uint32 column[480];
            int dark = 255 - g_settings.scanlines * 200 / 100;
            for (int y = 0; y < 480; y++) {
                int v = (y & 1) ? dark : 255;
                column[y] = 0xFF000000u | ((Uint32)v << 16) | ((Uint32)v << 8) | (Uint32)v;
            }
            SDL_UpdateTexture(g_app.scanline_texture, NULL, column, 4);
            SDL_SetTextureBlendMode(g_app.scanline_texture, SDL_BLENDMODE_MOD);
            SDL_SetTextureScaleMode(g_app.scanline_texture, SDL_ScaleModeLinear);
        }
    }
    if (g_settings.mask_type != MASK_NONE && g_settings.mask_strength > 0) {
        g_app.mask_texture = SDL_CreateTexture(g_app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        Uint32 *pixels = (Uint32 *)malloc((size_t)w * h * 4);
        if (g_app.mask_texture && pixels) {
            int low = 255 - g_settings.mask_strength * 160 / 100;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int phase = x % 3;
                    if (g_settings.mask_type == MASK_SLOT && ((y / 2) % 2)) phase = (x + 1) % 3;   /* staggered slots */
                    int r = phase == 0 ? 255 : low, g = phase == 1 ? 255 : low, b = phase == 2 ? 255 : low;
                    if (g_settings.mask_type == MASK_SLOT && (y % 4) == 3) { r = r * 3 / 4; g = g * 3 / 4; b = b * 3 / 4; }
                    pixels[y * w + x] = 0xFF000000u | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
                }
            }
            SDL_UpdateTexture(g_app.mask_texture, NULL, pixels, w * 4);
            SDL_SetTextureBlendMode(g_app.mask_texture, SDL_BLENDMODE_MOD);
        }
        free(pixels);
    }
}

static void game_rect(SDL_Rect *rect) {
    int rotated = g_settings.rotation == 90 || g_settings.rotation == 270;
    int src_w = rotated ? NSL_SCREEN_HEIGHT : NSL_SCREEN_WIDTH;
    int src_h = rotated ? NSL_SCREEN_WIDTH : NSL_SCREEN_HEIGHT;
    int w, h;
    if (g_settings.scale_mode == SCALE_INTEGER) {
        int k = g_app.width / src_w < g_app.height / src_h ? g_app.width / src_w : g_app.height / src_h;
        if (k < 1) k = 1;
        w = src_w * k;
        h = src_h * k;
    } else {
        /* 4:3 picture (square pixels on this board), as large as fits */
        w = g_app.width;
        h = w * src_h / src_w;
        if (h > g_app.height) { h = g_app.height; w = h * src_w / src_h; }
    }
    rect->w = w; rect->h = h;
    rect->x = (g_app.width - w) / 2;
    rect->y = (g_app.height - h) / 2;
}

static void upload_frame(void) {
    static uint8_t lut[256];
    static int lut_brightness = -1, lut_gamma = -1;
    if (lut_brightness != g_settings.brightness || lut_gamma != g_settings.gamma) {
        lut_brightness = g_settings.brightness;
        lut_gamma = g_settings.gamma;
        for (int i = 0; i < 256; i++) {
            double v = pow(i / 255.0, 100.0 / g_settings.gamma) * g_settings.brightness / 100.0;
            int out = (int)lround(v * 255.0);
            lut[i] = (uint8_t)(out > 255 ? 255 : out < 0 ? 0 : out);
        }
    }
    void *pixels;
    int pitch;
    if (SDL_LockTexture(g_app.game_texture, NULL, &pixels, &pitch) != 0) return;
    const uint32_t *src = nsl_core_framebuffer();
    int identity = g_settings.brightness == 100 && g_settings.gamma == 100;
    for (int y = 0; y < NSL_SCREEN_HEIGHT; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)pixels + y * pitch);
        const uint32_t *row = src + y * NSL_SCREEN_WIDTH;
        if (identity) {
            for (int x = 0; x < NSL_SCREEN_WIDTH; x++) dst[x] = row[x] | 0xFF000000u;
        } else {
            for (int x = 0; x < NSL_SCREEN_WIDTH; x++) {
                uint32_t p = row[x];
                dst[x] = 0xFF000000u | ((uint32_t)lut[(p >> 16) & 255] << 16) | ((uint32_t)lut[(p >> 8) & 255] << 8) | lut[p & 255];
            }
        }
    }
    SDL_UnlockTexture(g_app.game_texture);
}

void app_draw_background(void) {
    UiColor accent = ui_accents[g_settings.accent & 7];
    switch (g_settings.border) {
    case 0:
        ui_fill(0, 0, g_app.width, g_app.height, ui_rgba(0, 0, 0, 255));
        break;
    case 2:
        ui_gradient(0, 0, g_app.width, g_app.height, ui_mix(ui_rgba(10, 10, 14, 255), accent, 0.25f), ui_rgba(6, 6, 8, 255));
        break;
    case 3: {
        ui_fill(0, 0, g_app.width, g_app.height, ui_rgba(12, 12, 16, 255));
        UiColor line = ui_mix(ui_rgba(12, 12, 16, 255), accent, 0.18f);
        for (int x = 0; x < g_app.width; x += 32) ui_fill(x, 0, 1, g_app.height, line);
        for (int y = 0; y < g_app.height; y += 32) ui_fill(0, y, g_app.width, 1, line);
        break;
    }
    default:
        ui_gradient(0, 0, g_app.width, g_app.height, ui_rgba(22, 22, 30, 255), ui_rgba(6, 6, 9, 255));
        break;
    }
}

void app_draw_game(int dim) {
    SDL_Rect rect;
    game_rect(&rect);
    app_draw_background();
    SDL_SetTextureScaleMode(g_app.game_texture, g_settings.filter_linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    double angle = (double)g_settings.rotation;
    SDL_Rect draw = rect;
    if (g_settings.rotation == 90 || g_settings.rotation == 270) {
        /* RenderCopyEx rotates around the centre: give it the unrotated size */
        draw.w = rect.h; draw.h = rect.w;
        draw.x = rect.x + (rect.w - draw.w) / 2;
        draw.y = rect.y + (rect.h - draw.h) / 2;
    }
    SDL_RenderCopyEx(g_app.renderer, g_app.game_texture, NULL, &draw, angle, NULL, SDL_FLIP_NONE);
    rebuild_overlays(rect.w, rect.h);
    if (g_app.scanline_texture) SDL_RenderCopyEx(g_app.renderer, g_app.scanline_texture, NULL, &draw, angle, NULL, SDL_FLIP_NONE);
    if (g_app.mask_texture) SDL_RenderCopy(g_app.renderer, g_app.mask_texture, NULL, &rect);
    if (dim) ui_fill(0, 0, g_app.width, g_app.height, ui_rgba(0, 0, 0, 170));
}

static int ui_scale(void) {
    int s = g_app.height / 270;
    return s < 2 ? 2 : s;
}

void app_draw_gadgets(void) {
    SDL_Rect rect;
    game_rect(&rect);
    int scale_base = ui_scale();
    char text[96];
    UiColor white = ui_rgba(240, 240, 240, 230);
    UiColor accent = ui_accents[g_settings.accent & 7];
    struct { int on; const char *fmt; } list[3] = { { g_settings.show_fps, NULL }, { g_settings.show_frametime, NULL }, { g_settings.show_inputs, NULL } };
    for (int g = 0; g < 3; g++) {
        if (!list[g].on) continue;
        int scale = g_settings.gadget_scale[g] * scale_base / 2;
        if (scale < 1) scale = 1;
        int x = rect.x + rect.w * g_settings.gadget_x[g] / 100;
        int y = rect.y + rect.h * g_settings.gadget_y[g] / 100;
        if (g == 0) snprintf(text, sizeof(text), "%.1f FPS", g_app.fps);
        else if (g == 1) snprintf(text, sizeof(text), "%.2f ms", g_app.emu_ms);
        if (g < 2) {
            int w = ui_text_width(text, scale) + 8 * scale / 2;
            if (x + w > rect.x + rect.w) x = rect.x + rect.w - w;
            int h = 16 * scale / 2 + 4;
            if (y + h > rect.y + rect.h) y = rect.y + rect.h - h;
            ui_fill(x, y, w, h, ui_rgba(0, 0, 0, 140));
            ui_text(x + 4 * scale / 2, y + 2, text, scale, white);
        } else {
            uint16_t in = g_app.input.player[0];
            int cell = 8 * scale;
            int h = cell * 3 + 8;
            if (y + h > rect.y + rect.h) y = rect.y + rect.h - h;
            int w = cell * 8;
            if (x + w > rect.x + rect.w) x = rect.x + rect.w - w;
            ui_fill(x, y, w, h, ui_rgba(0, 0, 0, 140));
            int cx = x + cell + 4, cy = y + cell + 4;
            UiColor off = ui_rgba(70, 70, 80, 220);
            ui_fill(cx, cy - cell, cell, cell - 2, (in & NSL_IN_UP) ? accent : off);
            ui_fill(cx, cy + cell + 2, cell, cell - 2, (in & NSL_IN_DOWN) ? accent : off);
            ui_fill(cx - cell, cy, cell - 2, cell, (in & NSL_IN_LEFT) ? accent : off);
            ui_fill(cx + cell + 2, cy, cell - 2, cell, (in & NSL_IN_RIGHT) ? accent : off);
            const char *labels[3] = { "1", "2", "3" };
            uint16_t bits[3] = { NSL_IN_B1, NSL_IN_B2, NSL_IN_B3 };
            for (int b = 0; b < 3; b++) {
                int bx = x + cell * 4 + b * (cell + 4), by = cy;
                ui_fill(bx, by, cell, cell, (in & bits[b]) ? accent : off);
                ui_text(bx + cell / 4, by + cell / 8, labels[b], scale, white);
            }
        }
    }
}

/* ---- audio --------------------------------------------------------------------- */

void app_reopen_audio(void) {
    if (g_app.audio) SDL_CloseAudioDevice(g_app.audio);
    g_app.audio = 0;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = nsl_core_audio_rate();
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    const char *device = g_settings.audio_device[0] ? g_settings.audio_device : NULL;
    g_app.audio = SDL_OpenAudioDevice(device, 0, &want, &have, 0);
    if (!g_app.audio && device) {
        app_log("audio device \"%s\" unavailable (%s); using the default", device, SDL_GetError());
        g_app.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    }
    if (!g_app.audio) {
        app_log("no audio output: %s", SDL_GetError());
        return;
    }
    g_app.audio_rate = have.freq;
    SDL_PauseAudioDevice(g_app.audio, 0);
}

/* Queues the frame's audio with a small rate correction that keeps the queue near
 * the configured latency instead of growing or starving (no unbounded buffering). */
static void queue_audio(void) {
    if (!g_app.audio) return;
    const int16_t *samples;
    size_t count = nsl_core_audio(&samples);
    if (!count) return;
    Uint32 queued = SDL_GetQueuedAudioSize(g_app.audio) / 4;
    double target = g_settings.latency_ms * g_app.audio_rate / 1000.0;
    if (queued > target * 3) {           /* way behind (e.g. after a stall): drop the excess */
        SDL_ClearQueuedAudio(g_app.audio);
        queued = 0;
    }
    double error = (queued - target) / (target > 1 ? target : 1);
    if (error > 1) error = 1;
    if (error < -1) error = -1;
    double ratio = (double)nsl_core_audio_rate() / g_app.audio_rate * (1.0 + 0.005 * error);
    static int16_t out[8192];
    static double phase;
    float gain = g_settings.mute ? 0.0f : g_settings.volume / 100.0f;
    size_t produced = 0;
    while (phase < (double)(count - 1) && produced < sizeof(out) / 4) {
        size_t i = (size_t)phase;
        double frac = phase - i;
        for (int c = 0; c < 2; c++) {
            double v = samples[i * 2 + c] + (samples[(i + 1) * 2 + c] - samples[i * 2 + c]) * frac;
            out[produced * 2 + c] = (int16_t)(v * gain);
        }
        produced++;
        phase += ratio;
    }
    phase -= (double)(count - 1);
    if (phase < 0) phase = 0;
    SDL_QueueAudio(g_app.audio, out, (Uint32)(produced * 4));
}

/* ---- display mode ------------------------------------------------------------- */

void app_apply_window_mode(void) {
    SDL_SetWindowFullscreen(g_app.window, g_settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void app_apply_display(void) {
    SDL_RenderSetVSync(g_app.renderer, g_settings.vsync ? 1 : 0);
    app_apply_window_mode();
}

/* ---- input ---------------------------------------------------------------------- */

static int pad_slot_of(SDL_JoystickID id) {
    for (int i = 0; i < g_app.pad_count; i++) if (g_app.pad_ids[i] == id) return i;
    return -1;
}

static void open_pad(int device_index) {
    if (!SDL_IsGameController(device_index) || g_app.pad_count >= MAX_PADS) return;
    SDL_GameController *pad = SDL_GameControllerOpen(device_index);
    if (!pad) return;
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
    if (pad_slot_of(id) >= 0) { SDL_GameControllerClose(pad); return; }
    g_app.pads[g_app.pad_count] = pad;
    g_app.pad_ids[g_app.pad_count] = id;
    g_app.pad_count++;
    app_toast("Controller connected: %s", SDL_GameControllerName(pad) ? SDL_GameControllerName(pad) : "controller");
    app_log("controller %d: %s", g_app.pad_count - 1, SDL_GameControllerName(pad));
}

static void close_pad(SDL_JoystickID id) {
    int slot = pad_slot_of(id);
    if (slot < 0) return;
    SDL_GameControllerClose(g_app.pads[slot]);
    for (int i = slot; i < g_app.pad_count - 1; i++) {
        g_app.pads[i] = g_app.pads[i + 1];
        g_app.pad_ids[i] = g_app.pad_ids[i + 1];
    }
    g_app.pad_count--;
    release_all_inputs();
    app_toast("Controller disconnected");
    if (g_app.mode == MODE_GAME && !menus_active()) menus_open_quick();   /* pause safely */
}

static int pad_button(int pad, int button) {
    if (pad < 0 || pad >= g_app.pad_count || button < 0) return 0;
    return SDL_GameControllerGetButton(g_app.pads[pad], (SDL_GameControllerButton)button);
}

static int any_pad_button(int button) {
    for (int i = 0; i < g_app.pad_count; i++) if (pad_button(i, button)) return 1;
    return 0;
}

static int g_script_held[SDL_NUM_SCANCODES];   /* scripted test input (see script_step) */

/* Samples every bound control and builds this frame's board inputs. */
static void sample_game_input(void) {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    NslInputState in;
    memset(&in, 0, sizeof(in));
    static const uint16_t bits[ACT_PLAYER_COUNT] = {
        NSL_IN_UP, NSL_IN_DOWN, NSL_IN_LEFT, NSL_IN_RIGHT, NSL_IN_B1, NSL_IN_B2, NSL_IN_B3, NSL_IN_START, NSL_IN_COIN,
    };
    int any = 0;
    for (int p = 0; p < NSL_PLAYERS; p++) {
        int pad = g_settings.pad_index[p] < g_app.pad_count ? g_settings.pad_index[p] : -1;
        for (int a = 0; a < ACT_PLAYER_COUNT; a++) {
            int on = 0;
            int sc = g_settings.key[p][a];
            if (sc > 0 && sc < SDL_NUM_SCANCODES && (keys[sc] || g_script_held[sc] > 0)) on = 1;
            if (a != ACT_COIN && pad >= 0 && pad_button(pad, g_settings.pad_button[p][a])) on = 1;
            if (on) { in.player[p] |= bits[a]; any = 1; }
        }
        if (pad >= 0 && g_settings.analog_stick) {
            int dz = 32767 * g_settings.deadzone / 100;
            int ax = SDL_GameControllerGetAxis(g_app.pads[pad], SDL_CONTROLLER_AXIS_LEFTX);
            int ay = SDL_GameControllerGetAxis(g_app.pads[pad], SDL_CONTROLLER_AXIS_LEFTY);
            if (ax < -dz) in.player[p] |= NSL_IN_LEFT;
            if (ax > dz) in.player[p] |= NSL_IN_RIGHT;
            if (ay < -dz) in.player[p] |= NSL_IN_UP;
            if (ay > dz) in.player[p] |= NSL_IN_DOWN;
            if (ax < -dz || ax > dz || ay < -dz || ay > dz) any = 1;
        }
        /* controller coin: Back pressed and released on its own (Back+Start opens the menu) */
        if (pad >= 0 && g_settings.pad_button[p][ACT_COIN] >= 0) {
            static int back_down[NSL_PLAYERS], back_clean[NSL_PLAYERS];
            int down = pad_button(pad, g_settings.pad_button[p][ACT_COIN]);
            if (down && !back_down[p]) back_clean[p] = 1;
            if (down && (in.player[p] & ~NSL_IN_COIN)) back_clean[p] = 0;
            if (!down && back_down[p] && back_clean[p]) g_app.coin_pulse[p] = 4;
            back_down[p] = down;
            if (down) any = 1;
        }
        if (g_app.coin_pulse[p] > 0) { in.player[p] |= NSL_IN_COIN; g_app.coin_pulse[p]--; }
        /* opposite directions cannot both be closed on a real stick */
        if ((in.player[p] & (NSL_IN_LEFT | NSL_IN_RIGHT)) == (NSL_IN_LEFT | NSL_IN_RIGHT)) in.player[p] &= (uint16_t)~(NSL_IN_LEFT | NSL_IN_RIGHT);
        if ((in.player[p] & (NSL_IN_UP | NSL_IN_DOWN)) == (NSL_IN_UP | NSL_IN_DOWN)) in.player[p] &= (uint16_t)~(NSL_IN_UP | NSL_IN_DOWN);
    }
    int sc = g_settings.hot_key[HOT_TEST];
    if ((sc > 0 && keys[sc]) || any_pad_button(g_settings.hot_pad[HOT_TEST])) in.system |= NSL_SYS_TEST;
    if (g_app.service_pulse > 0) { in.system |= NSL_SYS_SERVICE_COIN; g_app.service_pulse--; }
    for (int k = 0; k < SDL_NUM_SCANCODES; k++) if (g_script_held[k] > 0) g_script_held[k]--;
    if (g_app.suppress_game_input) {
        if (!any) g_app.suppress_game_input = 0;
        memset(&in, 0, sizeof(in));
    }
    g_app.input = in;
}

static int hot_key_pressed(const SDL_Event *e, int hot) {
    return e->type == SDL_KEYDOWN && !e->key.repeat && g_settings.hot_key[hot] > 0
        && (int)e->key.keysym.scancode == g_settings.hot_key[hot];
}

static int hot_pad_pressed(const SDL_Event *e, int hot) {
    return e->type == SDL_CONTROLLERBUTTONDOWN && g_settings.hot_pad[hot] >= 0
        && e->cbutton.button == g_settings.hot_pad[hot];
}

static int menu_combo_pressed(const SDL_Event *e) {
    if (e->type != SDL_CONTROLLERBUTTONDOWN) return 0;
    if ((g_settings.menu_combo == 1 || g_settings.menu_combo == 2) && e->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) return 1;
    if (g_settings.menu_combo == 0 || g_settings.menu_combo == 2) {
        int slot = pad_slot_of(e->cbutton.which);
        if (slot < 0) return 0;
        if ((e->cbutton.button == SDL_CONTROLLER_BUTTON_START && pad_button(slot, SDL_CONTROLLER_BUTTON_BACK))
            || (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK && pad_button(slot, SDL_CONTROLLER_BUTTON_START))) return 1;
    }
    return 0;
}

static void game_hotkeys(const SDL_Event *e) {
    if (hot_key_pressed(e, HOT_MENU) || menu_combo_pressed(e)) {
        menus_open_quick();
        release_all_inputs();
        return;
    }
    int back_held = 0;
    if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        int slot = pad_slot_of(e->cbutton.which);
        back_held = slot >= 0 && pad_button(slot, SDL_CONTROLLER_BUTTON_BACK);
    }
    if (hot_key_pressed(e, HOT_QUICK_SAVE) || (back_held && hot_pad_pressed(e, HOT_QUICK_SAVE))) app_save_state(g_settings.slot);
    if (hot_key_pressed(e, HOT_QUICK_LOAD) || (back_held && hot_pad_pressed(e, HOT_QUICK_LOAD))) app_load_state(g_settings.slot);
    if (hot_key_pressed(e, HOT_NEXT_SLOT) || hot_pad_pressed(e, HOT_NEXT_SLOT)) {
        g_settings.slot = (g_settings.slot + 1) % NSL_SLOTS;
        app_toast("Slot %d%s", g_settings.slot + 1, app_state_exists(g_settings.slot) ? "" : " (empty)");
    }
    if (hot_key_pressed(e, HOT_PREV_SLOT) || hot_pad_pressed(e, HOT_PREV_SLOT)) {
        g_settings.slot = (g_settings.slot + NSL_SLOTS - 1) % NSL_SLOTS;
        app_toast("Slot %d%s", g_settings.slot + 1, app_state_exists(g_settings.slot) ? "" : " (empty)");
    }
    if (hot_key_pressed(e, HOT_SCREENSHOT) || hot_pad_pressed(e, HOT_SCREENSHOT)) app_screenshot();
    if (hot_key_pressed(e, HOT_PAUSE) || hot_pad_pressed(e, HOT_PAUSE)) {
        g_app.paused = !g_app.paused;
        app_toast(g_app.paused ? "Paused" : "Resumed");
        if (!g_app.paused) g_app.next_frame = SDL_GetPerformanceCounter();
    }
    if (hot_key_pressed(e, HOT_SERVICE) || hot_pad_pressed(e, HOT_SERVICE)) g_app.service_pulse = 4;
}

static int rewind_held(void) {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int sc = g_settings.hot_key[HOT_REWIND];
    return (sc > 0 && keys[sc]) || any_pad_button(g_settings.hot_pad[HOT_REWIND]);
}

/* menu navigation from keyboard, mouse and every controller */
static void menu_event(const SDL_Event *e, MenuInput *mi) {
    switch (e->type) {
    case SDL_KEYDOWN:
        mi->any_key_scancode = e->key.keysym.scancode;
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: mi->up = 1; break;
        case SDL_SCANCODE_DOWN: mi->down = 1; break;
        case SDL_SCANCODE_LEFT: mi->left = 1; break;
        case SDL_SCANCODE_RIGHT: mi->right = 1; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
            if (!e->key.repeat) mi->accept = 1;
            break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE:
            if (!e->key.repeat) mi->back = 1;
            break;
        case SDL_SCANCODE_PAGEUP: mi->page_up = 1; break;
        case SDL_SCANCODE_PAGEDOWN: mi->page_down = 1; break;
        case SDL_SCANCODE_DELETE: mi->delete_key = 1; break;
        default: break;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        mi->any_pad_button = e->cbutton.button;
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: mi->up = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: mi->down = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: mi->left = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: mi->right = 1; break;
        case SDL_CONTROLLER_BUTTON_A: mi->accept = 1; break;
        case SDL_CONTROLLER_BUTTON_B: mi->back = 1; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: mi->page_up = 1; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: mi->page_down = 1; break;
        case SDL_CONTROLLER_BUTTON_Y: mi->delete_key = 1; break;
        default: break;
        }
        break;
    case SDL_CONTROLLERAXISMOTION: {
        static int last_y[MAX_PADS], last_x[MAX_PADS];
        int slot = pad_slot_of(e->caxis.which);
        if (slot < 0) break;
        int v = e->caxis.value, th = 20000;
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            int state = v < -th ? -1 : v > th ? 1 : 0;
            if (state != last_y[slot]) { if (state < 0) mi->up = 1; if (state > 0) mi->down = 1; }
            last_y[slot] = state;
        } else if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            int state = v < -th ? -1 : v > th ? 1 : 0;
            if (state != last_x[slot]) { if (state < 0) mi->left = 1; if (state > 0) mi->right = 1; }
            last_x[slot] = state;
        }
        break;
    }
    case SDL_MOUSEMOTION: {
        /* the first motion (pointer already over the window) only sets the reference, so
         * a resting cursor never steals keyboard/controller focus */
        static int have_reference, ref_x, ref_y;
        mi->mouse_x = e->motion.x; mi->mouse_y = e->motion.y;
        if (have_reference && (abs(e->motion.x - ref_x) + abs(e->motion.y - ref_y)) >= 4) {
            mi->mouse_moved = 1;
            ref_x = e->motion.x; ref_y = e->motion.y;
        } else if (!have_reference) {
            have_reference = 1; ref_x = e->motion.x; ref_y = e->motion.y;
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        mi->mouse_x = e->button.x; mi->mouse_y = e->button.y;
        if (e->button.button == SDL_BUTTON_LEFT) mi->mouse_click = 1;
        if (e->button.button == SDL_BUTTON_RIGHT) mi->back = 1;
        break;
    case SDL_MOUSEWHEEL:
        mi->mouse_wheel += e->wheel.y;
        break;
    default:
        break;
    }
}

/* ---- scripted UI test driver (NSL_UI_SCRIPT=file) ----------------------------
 * Lines: "wait N" (loop iterations), "key NAME" (SDL scancode name), "pad BUTTON",
 * "shot FILE" (PNG of the whole window), "quit". Used by tests/ui_smoke.sh. */
static char **g_script;
static int g_script_count, g_script_pos, g_script_wait;
static char g_shot_path[1024];

static void script_load(void) {
    const char *path = getenv("NSL_UI_SCRIPT");
    if (!path) return;
    size_t size = 0;
    char *text = (char *)nsl_read_file(path, &size);
    if (!text) return;
    text = (char *)realloc(text, size + 1);
    text[size] = 0;
    g_script = (char **)calloc(4096, sizeof(char *));
    for (char *line = strtok(text, "\n"); line && g_script_count < 4096; line = strtok(NULL, "\n")) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (n && line[0] != '#') g_script[g_script_count++] = line;
    }
}

static void script_step(void) {
    if (!g_script || g_script_pos >= g_script_count) return;
    if (g_script_wait > 0) { g_script_wait--; return; }
    char *line = g_script[g_script_pos++];
    SDL_Event e;
    SDL_zero(e);
    if (!strncmp(line, "wait ", 5)) g_script_wait = atoi(line + 5);
    else if (!strncmp(line, "key ", 4)) {
        SDL_Scancode sc = SDL_GetScancodeFromName(line + 4);
        e.type = SDL_KEYDOWN; e.key.keysym.scancode = sc; e.key.keysym.sym = SDL_GetKeyFromScancode(sc); e.key.state = SDL_PRESSED;
        SDL_PushEvent(&e);
        SDL_Event up = e; up.type = SDL_KEYUP; up.key.state = SDL_RELEASED;
        SDL_PushEvent(&up);
    } else if (!strncmp(line, "pad ", 4)) {
        e.type = SDL_CONTROLLERBUTTONDOWN; e.cbutton.button = (Uint8)SDL_GameControllerGetButtonFromString(line + 4);
        e.cbutton.which = -1;
        SDL_PushEvent(&e);
    } else if (!strncmp(line, "shot ", 5)) snprintf(g_shot_path, sizeof(g_shot_path), "%s", line + 5);
    else if (!strcmp(line, "quit")) { SDL_Event q; SDL_zero(q); q.type = SDL_QUIT; SDL_PushEvent(&q); }
}

static void script_capture(void) {
    if (!g_shot_path[0]) return;
    int w = g_app.width, h = g_app.height;
    uint32_t *pixels = (uint32_t *)malloc((size_t)w * h * 4);
    if (pixels && SDL_RenderReadPixels(g_app.renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels, w * 4) == 0) {
        uint8_t *png = NULL;
        size_t size = 0;
        if (nsl_core_png_encode(pixels, w, h, &png, &size) == 0) nsl_write_file_atomic(g_shot_path, png, size);
        free(png);
    }
    free(pixels);
    g_shot_path[0] = 0;
}

/* ---- startup ------------------------------------------------------------------- */

static void find_exe_dir(void) {
    char *base = SDL_GetBasePath();
    if (base) {
        snprintf(g_app.exe_dir, sizeof(g_app.exe_dir), "%s", base);
        SDL_free(base);
    } else {
        snprintf(g_app.exe_dir, sizeof(g_app.exe_dir), ".");
    }
}

static int try_rom(const char *path) {
    if (!path || !path[0] || !nsl_path_exists(path)) return -1;
    return app_load_rom(path);
}

static void find_rom(const char *cli) {
    if (cli) { try_rom(cli); return; }               /* command line wins; errors are shown */
    if (try_rom(g_settings.rom_path) == 0) return;
    char path[1400];
    const char *appimage = getenv("APPIMAGE");
    if (appimage) {                                   /* next to the AppImage file itself */
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", appimage);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; nsl_path_join(path, sizeof(path), dir, "nslasher.zip"); if (try_rom(path) == 0) return; }
    }
    const char *bases[3] = { g_app.exe_dir, g_app.data_dir, "." };
    for (int b = 0; b < 3; b++) {
        char rom_dir[1200];
        nsl_path_join(rom_dir, sizeof(rom_dir), bases[b], "rom");
        nsl_path_join(path, sizeof(path), rom_dir, "nslasher.zip");
        if (try_rom(path) == 0) return;
        nsl_path_join(path, sizeof(path), bases[b], "nslasher.zip");
        if (try_rom(path) == 0) return;
    }
    if (!g_app.rom_status[0]) snprintf(g_app.rom_status, sizeof(g_app.rom_status), "No ROM selected yet.");
}

static void write_missed_blocks(void) {
    const uint32_t *missed;
    unsigned count = nsl_interp_missed(&missed);
    if (!count) return;
    char path[1200];
    nsl_path_join(path, sizeof(path), g_app.data_dir, "missed_blocks.txt");
    FILE *file = nsl_fopen(path, "a");
    if (!file) return;
    for (unsigned i = 0; i < count; i++) fprintf(file, "%06X\n", missed[i]);
    fclose(file);
}

int main(int argc, char **argv) {
    const char *cli_rom = NULL;
    int cli_fullscreen = -1, cli_start = 0, cli_w = 0, cli_h = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) cli_rom = argv[++i];
        else if (!strcmp(argv[i], "--fullscreen")) cli_fullscreen = 1;
        else if (!strcmp(argv[i], "--windowed")) cli_fullscreen = 0;
        else if (!strcmp(argv[i], "--start")) cli_start = 1;
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) { sscanf(argv[++i], "%dx%d", &cli_w, &cli_h); cli_fullscreen = 0; }
        else if (argv[i][0] != '-') cli_rom = argv[i];
    }

    snprintf(g_app.data_dir, sizeof(g_app.data_dir), "%s", nsl_user_data_dir());
    nsl_path_join(g_app.log_path, sizeof(g_app.log_path), g_app.data_dir, "nightslashers.log");
    char states_root[1100];
    nsl_path_join(states_root, sizeof(states_root), g_app.data_dir, "states");
    nsl_path_join(g_app.state_dir, sizeof(g_app.state_dir), states_root, "nslasher");
    nsl_make_directories(g_app.state_dir);

    settings_defaults(&g_settings);
    char settings_file[1100];
    settings_path(settings_file, sizeof(settings_file));
    settings_load(&g_settings, settings_file);
    if (cli_fullscreen >= 0) g_settings.fullscreen = cli_fullscreen;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    find_exe_dir();
    app_log("Night Slashers Recomp starting; data folder %s; SDL video driver %s",
            g_app.data_dir, SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");

    int ws = g_settings.window_scale;
    g_app.window = SDL_CreateWindow("Night Slashers", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    cli_w > 0 ? cli_w : 320 * ws, cli_h > 0 ? cli_h : 240 * ws, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
                                    | (g_settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!g_app.window) { app_log("SDL_CreateWindow: %s", SDL_GetError()); return 1; }
    g_app.renderer = SDL_CreateRenderer(g_app.window, -1, SDL_RENDERER_ACCELERATED | (g_settings.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!g_app.renderer) g_app.renderer = SDL_CreateRenderer(g_app.window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_app.renderer) { app_log("SDL_CreateRenderer: %s", SDL_GetError()); return 1; }
    g_app.game_texture = SDL_CreateTexture(g_app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                           NSL_SCREEN_WIDTH, NSL_SCREEN_HEIGHT);
    ui_init(g_app.renderer);
    SDL_ShowCursor(SDL_ENABLE);

    for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
    g_app.toast_until = 0;

    find_rom(cli_rom);
    app_reopen_audio();
    app_rewind_configure();
    menus_init();

    g_app.perf_freq = SDL_GetPerformanceFrequency();
    g_app.frame_period = 1.0 / NSL_FRAME_RATE;
    g_app.next_frame = SDL_GetPerformanceCounter();
    g_app.fps_start = g_app.next_frame;
    g_app.running = 1;
    g_app.mode = MODE_MENU;
    menus_open_main();
    if (cli_start && g_app.rom_ok) { app_start_game(); }
    script_load();

    while (g_app.running) {
        MenuInput mi;
        memset(&mi, 0, sizeof(mi));
        mi.any_key_scancode = -1;
        mi.any_pad_button = -1;
        SDL_GetMouseState(&mi.mouse_x, &mi.mouse_y);
        SDL_GetRendererOutputSize(g_app.renderer, &g_app.width, &g_app.height);
        {   /* mouse coordinates are in window units; scale them to renderer pixels (HiDPI) */
            int ww, wh;
            SDL_GetWindowSize(g_app.window, &ww, &wh);
            if (ww > 0 && wh > 0) { mi.mouse_x = mi.mouse_x * g_app.width / ww; mi.mouse_y = mi.mouse_y * g_app.height / wh; }
        }

        script_step();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { app_quit(); break; }
            if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
            if (e.type == SDL_CONTROLLERDEVICEREMOVED) close_pad(e.cdevice.which);
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) release_all_inputs();
            if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN) {
                int ww, wh;
                SDL_GetWindowSize(g_app.window, &ww, &wh);
                if (e.type == SDL_MOUSEMOTION && ww > 0) { e.motion.x = e.motion.x * g_app.width / ww; e.motion.y = e.motion.y * g_app.height / wh; }
                if (e.type == SDL_MOUSEBUTTONDOWN && ww > 0) { e.button.x = e.button.x * g_app.width / ww; e.button.y = e.button.y * g_app.height / wh; }
            }
            if (hot_key_pressed(&e, HOT_FULLSCREEN)) {
                g_settings.fullscreen = !g_settings.fullscreen;
                app_apply_window_mode();
                continue;
            }
            if (menus_active()) menu_event(&e, &mi);
            else if (g_app.mode == MODE_GAME) game_hotkeys(&e);
        }
        if (!g_app.running) break;

        if (menus_active()) {
            menus_update(&mi);
            g_app.next_frame = SDL_GetPerformanceCounter();
        } else if (g_app.mode == MODE_GAME && g_app.game_active) {
            uint64_t now = SDL_GetPerformanceCounter();
            uint64_t period = (uint64_t)(g_app.frame_period * g_app.perf_freq);
            int steps = 0;
            while (now >= g_app.next_frame && steps < 3 && !g_app.paused) {
                sample_game_input();
                g_app.rewinding = g_app.rewind_capacity && rewind_held();
                if (g_app.rewinding) {
                    if (rewind_pop() != 0) g_app.rewinding = 0;
                    if (g_app.audio) SDL_ClearQueuedAudio(g_app.audio);
                } else {
                    NslInputState frame_input = g_app.input;
                    if (g_app.playing_back) {
                        if (g_app.rec_position < g_app.rec_count) frame_input = g_app.rec_inputs[g_app.rec_position++];
                        else { g_app.playing_back = 0; app_toast("Recording finished"); }
                    } else if (g_app.recording) {
                        if (g_app.rec_count >= g_app.rec_capacity) {
                            int capacity = g_app.rec_capacity ? g_app.rec_capacity * 2 : 4096;
                            NslInputState *grown = (NslInputState *)realloc(g_app.rec_inputs, (size_t)capacity * sizeof(NslInputState));
                            if (grown) { g_app.rec_inputs = grown; g_app.rec_capacity = capacity; }
                        }
                        if (g_app.rec_count < g_app.rec_capacity) g_app.rec_inputs[g_app.rec_count++] = frame_input;
                    }
                    g_app.input = frame_input;
                    uint64_t t0 = SDL_GetPerformanceCounter();
                    if (nsl_core_run_frame(&frame_input) != 0) {
                        app_log("CPU fault: %s", nsl_core_fault_text());
                        app_toast("The game stopped: %s", nsl_core_fault_text());
                        g_app.game_active = 0;
                        write_missed_blocks();
                        g_app.mode = MODE_MENU;
                        menus_open_main();
                        break;
                    }
                    g_app.emu_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / g_app.perf_freq;
                    queue_audio();
                    rewind_push();
                    if (nsl_core_frame_number() >= g_app.eeprom_check_frame) {
                        app_save_eeprom();
                        g_app.eeprom_check_frame = nsl_core_frame_number() + 30 * 58;
                    }
                }
                g_app.next_frame += period;
                steps++;
                g_app.fps_frames++;
            }
            if (now > g_app.next_frame + period * 4) g_app.next_frame = now;   /* resync after a stall */
            if (now - g_app.fps_start > g_app.perf_freq) {
                g_app.fps = g_app.fps_frames * (double)g_app.perf_freq / (now - g_app.fps_start);
                g_app.fps_frames = 0;
                g_app.fps_start = now;
            }
        }

        /* draw */
        SDL_SetRenderDrawColor(g_app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_app.renderer);
        if (g_app.game_active) {
            upload_frame();
            app_draw_game(menus_active());
            if (!menus_active()) app_draw_gadgets();
        }
        if (menus_active()) menus_draw();
        if (g_app.paused && !menus_active()) ui_text_center(g_app.width / 2, g_app.height / 2, "PAUSED", ui_scale() + 1, ui_rgba(255, 255, 255, 255));
        if (g_app.rewinding) ui_text_shadow(16, 16, "<< REWIND", ui_scale(), ui_accents[g_settings.accent & 7]);
        if (g_app.recording) ui_text_shadow(16, 16, "REC", ui_scale(), ui_rgba(255, 60, 60, 255));
        if (g_app.playing_back) ui_text_shadow(16, 16, "PLAYBACK", ui_scale(), ui_rgba(120, 200, 255, 255));
        if (app_now_ms() < g_app.toast_until) {
            int s = ui_scale();
            int w = ui_text_width(g_app.toast, s) + 32;
            int h = 16 * s / 2 + 20;
            int x = (g_app.width - w) / 2, y = g_app.height - h - g_app.height / 16;
            ui_fill(x, y, w, h, ui_rgba(10, 10, 14, 220));
            ui_fill(x, y, 4, h, ui_accents[g_settings.accent & 7]);
            ui_text(x + 18, y + 10, g_app.toast, s, ui_rgba(240, 240, 240, 255));
        }
        script_capture();
        SDL_RenderPresent(g_app.renderer);
        if (!g_settings.vsync || menus_active() || g_app.paused) SDL_Delay(1);
    }

    write_missed_blocks();
    save_settings();
    app_save_eeprom();
    if (g_app.audio) SDL_CloseAudioDevice(g_app.audio);
    ui_shutdown();
    nsl_core_unload();
    SDL_DestroyRenderer(g_app.renderer);
    SDL_DestroyWindow(g_app.window);
    SDL_Quit();
    return 0;
}
