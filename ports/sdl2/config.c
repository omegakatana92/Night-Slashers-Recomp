/*
 * Night Slashers front end: persistent settings.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "config.h"
#include "nsl/platform.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings;

static const char *k_actions[ACT_PLAYER_COUNT] = {
    "up", "down", "left", "right", "attack", "jump", "special", "start", "coin",
};
static const char *k_hotkeys[HOT_COUNT] = {
    "menu", "quick_save", "quick_load", "next_slot", "prev_slot", "screenshot",
    "rewind", "pause", "service_coin", "test_mode", "fullscreen",
};

const char *action_name(int action) {
    static const char *pretty[ACT_PLAYER_COUNT] = {
        "Up", "Down", "Left", "Right", "Attack (B1)", "Jump (B2)", "Special (B3)", "Start", "Coin",
    };
    return (action >= 0 && action < ACT_PLAYER_COUNT) ? pretty[action] : "?";
}

const char *hotkey_name(int hotkey) {
    static const char *pretty[HOT_COUNT] = {
        "Quick menu", "Quick save", "Quick load", "Next slot", "Previous slot", "Screenshot",
        "Rewind (hold)", "Pause", "Service coin", "Test mode", "Fullscreen",
    };
    return (hotkey >= 0 && hotkey < HOT_COUNT) ? pretty[hotkey] : "?";
}

void settings_default_controls(Settings *s, int preset) {
    static const int p1[ACT_PLAYER_COUNT] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_1, SDL_SCANCODE_5,
    };
    static const int p2[ACT_PLAYER_COUNT] = {
        SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
        SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_2, SDL_SCANCODE_6,
    };
    for (int p = 0; p < NSL_PLAYERS; p++) {
        for (int a = 0; a < ACT_PLAYER_COUNT; a++) {
            s->key[p][a] = p == 0 ? p1[a] : p == 1 ? p2[a] : SDL_SCANCODE_UNKNOWN;
        }
        /* standard: A attack, B jump, X/Y special; arcade stick: X attack, A jump, Y special */
        s->pad_button[p][ACT_UP] = SDL_CONTROLLER_BUTTON_DPAD_UP;
        s->pad_button[p][ACT_DOWN] = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        s->pad_button[p][ACT_LEFT] = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        s->pad_button[p][ACT_RIGHT] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        if (preset == 1) {
            s->pad_button[p][ACT_B1] = SDL_CONTROLLER_BUTTON_X;
            s->pad_button[p][ACT_B2] = SDL_CONTROLLER_BUTTON_A;
            s->pad_button[p][ACT_B3] = SDL_CONTROLLER_BUTTON_Y;
        } else {
            s->pad_button[p][ACT_B1] = SDL_CONTROLLER_BUTTON_A;
            s->pad_button[p][ACT_B2] = SDL_CONTROLLER_BUTTON_B;
            s->pad_button[p][ACT_B3] = SDL_CONTROLLER_BUTTON_X;
        }
        s->pad_button[p][ACT_START] = SDL_CONTROLLER_BUTTON_START;
        s->pad_button[p][ACT_COIN] = SDL_CONTROLLER_BUTTON_BACK;
        s->pad_index[p] = p;
    }
    static const int hot_keys[HOT_COUNT] = {
        SDL_SCANCODE_ESCAPE, SDL_SCANCODE_F5, SDL_SCANCODE_F8, SDL_SCANCODE_F7, SDL_SCANCODE_F6,
        SDL_SCANCODE_F12, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_P, SDL_SCANCODE_9, SDL_SCANCODE_F2,
        SDL_SCANCODE_F11,
    };
    for (int h = 0; h < HOT_COUNT; h++) {
        s->hot_key[h] = hot_keys[h];
        s->hot_pad[h] = -1;
    }
    /* Controller hotkeys default to the shoulder buttons, which the game does not use. */
    s->hot_pad[HOT_QUICK_SAVE] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    s->hot_pad[HOT_QUICK_LOAD] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    s->hot_pad[HOT_REWIND] = SDL_CONTROLLER_BUTTON_LEFTSTICK;
}

void settings_defaults(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->fullscreen = getenv("SteamDeck") || getenv("SteamGamepadUI") ? 1 : 0;
    s->scale_mode = SCALE_FIT;
    s->filter_linear = 0;
    s->vsync = 1;
    s->rotation = 0;
    s->scanlines = 0;
    s->mask_type = MASK_NONE;
    s->mask_strength = 30;
    s->brightness = 100;
    s->gamma = 100;
    s->border = 1;
    s->accent = 0;
    s->window_scale = 3;
    s->volume = 100;
    s->mute = 0;
    s->latency_ms = 64;
    s->menu_combo = 2;
    s->deadzone = 35;
    s->analog_stick = 1;
    s->autosave = 1;
    s->rewind_seconds = 0;
    s->confirm_overwrite = 1;
    s->slot = 0;
    s->show_fps = 0;
    s->show_frametime = 0;
    s->show_inputs = 0;
    for (int g = 0; g < 3; g++) {
        s->gadget_x[g] = g == 2 ? 2 : 98;     /* percent of the screen */
        s->gadget_y[g] = 2 + g * 6;
        s->gadget_scale[g] = 2;
    }
    s->gadget_y[2] = 96;
    settings_default_controls(s, 0);
}

/* ---- tiny INI: "key = value" lines, [section] headers ignored for lookup ---- */

typedef struct Field {
    const char *name;
    int *value;
    int min, max;
} Field;

static int fields(Settings *s, Field *out) {
    int n = 0;
#define F(nm, ptr, lo, hi) do { out[n].name = nm; out[n].value = (ptr); out[n].min = lo; out[n].max = hi; n++; } while (0)
    F("fullscreen", &s->fullscreen, 0, 1);
    F("scale_mode", &s->scale_mode, 0, 1);
    F("filter_linear", &s->filter_linear, 0, 1);
    F("vsync", &s->vsync, 0, 1);
    F("rotation", &s->rotation, 0, 270);
    F("scanlines", &s->scanlines, 0, 100);
    F("mask_type", &s->mask_type, 0, 2);
    F("mask_strength", &s->mask_strength, 0, 100);
    F("brightness", &s->brightness, 50, 150);
    F("gamma", &s->gamma, 50, 200);
    F("border", &s->border, 0, 3);
    F("accent", &s->accent, 0, 7);
    F("window_scale", &s->window_scale, 1, 6);
    F("volume", &s->volume, 0, 100);
    F("mute", &s->mute, 0, 1);
    F("latency_ms", &s->latency_ms, 32, 200);
    F("menu_combo", &s->menu_combo, 0, 2);
    F("deadzone", &s->deadzone, 0, 90);
    F("analog_stick", &s->analog_stick, 0, 1);
    F("autosave", &s->autosave, 0, 1);
    F("rewind_seconds", &s->rewind_seconds, 0, 60);
    F("confirm_overwrite", &s->confirm_overwrite, 0, 1);
    F("slot", &s->slot, 0, NSL_SLOTS - 1);
    F("show_fps", &s->show_fps, 0, 1);
    F("show_frametime", &s->show_frametime, 0, 1);
    F("show_inputs", &s->show_inputs, 0, 1);
    for (int g = 0; g < 3; g++) {
        static char nx[3][16], ny[3][16], ns[3][16];
        snprintf(nx[g], sizeof(nx[g]), "gadget%d_x", g);
        snprintf(ny[g], sizeof(ny[g]), "gadget%d_y", g);
        snprintf(ns[g], sizeof(ns[g]), "gadget%d_scale", g);
        F(nx[g], &s->gadget_x[g], 0, 100);
        F(ny[g], &s->gadget_y[g], 0, 100);
        F(ns[g], &s->gadget_scale[g], 1, 4);
    }
    static char kn[NSL_PLAYERS][ACT_PLAYER_COUNT][32], pn[NSL_PLAYERS][ACT_PLAYER_COUNT][32];
    for (int p = 0; p < NSL_PLAYERS; p++) {
        static char pi[NSL_PLAYERS][24];
        snprintf(pi[p], sizeof(pi[p]), "p%d_controller", p + 1);
        F(pi[p], &s->pad_index[p], -1, 7);
        for (int a = 0; a < ACT_PLAYER_COUNT; a++) {
            snprintf(kn[p][a], sizeof(kn[p][a]), "p%d_key_%s", p + 1, k_actions[a]);
            snprintf(pn[p][a], sizeof(pn[p][a]), "p%d_pad_%s", p + 1, k_actions[a]);
            F(kn[p][a], &s->key[p][a], 0, SDL_NUM_SCANCODES - 1);
            F(pn[p][a], &s->pad_button[p][a], -1, SDL_CONTROLLER_BUTTON_MAX - 1);
        }
    }
    static char hk[HOT_COUNT][32], hp[HOT_COUNT][32];
    for (int h = 0; h < HOT_COUNT; h++) {
        snprintf(hk[h], sizeof(hk[h]), "hotkey_%s", k_hotkeys[h]);
        snprintf(hp[h], sizeof(hp[h]), "hotpad_%s", k_hotkeys[h]);
        F(hk[h], &s->hot_key[h], 0, SDL_NUM_SCANCODES - 1);
        F(hp[h], &s->hot_pad[h], -1, SDL_CONTROLLER_BUTTON_MAX - 1);
    }
#undef F
    return n;
}

static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') text++;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r'
                      || text[length - 1] == '\n')) text[--length] = 0;
    return text;
}

int settings_load(Settings *s, const char *path) {
    size_t size = 0;
    char *data = (char *)nsl_read_file(path, &size);
    if (!data) return -1;
    char *text = (char *)realloc(data, size + 1);
    if (!text) { free(data); return -1; }
    text[size] = 0;
    Field list[256];
    int count = fields(s, list);
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *equals = strchr(line, '=');
        if (!equals || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        *equals = 0;
        char *key = trim(line), *value = trim(equals + 1);
        if (!strcmp(key, "rom_path")) { snprintf(s->rom_path, sizeof(s->rom_path), "%s", value); continue; }
        if (!strcmp(key, "browse_dir")) { snprintf(s->browse_dir, sizeof(s->browse_dir), "%s", value); continue; }
        if (!strcmp(key, "audio_device")) { snprintf(s->audio_device, sizeof(s->audio_device), "%s", value); continue; }
        for (int i = 0; i < count; i++) {
            if (!strcmp(key, list[i].name)) {
                int v = atoi(value);
                if (v < list[i].min) v = list[i].min;
                if (v > list[i].max) v = list[i].max;
                *list[i].value = v;
                break;
            }
        }
    }
    free(text);
    if (s->rotation % 90) s->rotation = 0;
    return 0;
}

int settings_save(const Settings *s_in, const char *path) {
    Settings copy = *s_in;
    Field list[256];
    int count = fields(&copy, list);
    size_t capacity = 32768, used = 0;
    char *out = (char *)malloc(capacity);
    if (!out) return -1;
    used += (size_t)snprintf(out + used, capacity - used,
                             "# Night Slashers Recomp settings (UTF-8). Delete this file to restore defaults.\n"
                             "rom_path = %s\nbrowse_dir = %s\naudio_device = %s\n",
                             copy.rom_path, copy.browse_dir, copy.audio_device);
    for (int i = 0; i < count && used < capacity - 64; i++) {
        used += (size_t)snprintf(out + used, capacity - used, "%s = %d\n", list[i].name, *list[i].value);
    }
    int result = nsl_write_file_atomic(path, out, used);
    free(out);
    return result;
}
