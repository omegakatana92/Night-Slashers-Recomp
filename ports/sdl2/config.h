/*
 * Night Slashers front end: persistent settings (settings.ini in the user data folder).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_FE_CONFIG_H
#define NSL_FE_CONFIG_H

#include <stdint.h>

enum { SCALE_FIT = 0, SCALE_INTEGER = 1 };
enum { MASK_NONE = 0, MASK_APERTURE = 1, MASK_SLOT = 2 };

/* game actions a binding can drive */
enum {
    ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_B1, ACT_B2, ACT_B3, ACT_START, ACT_COIN,
    ACT_PLAYER_COUNT,
};
/* front-end hotkeys (keyboard and controller) */
enum {
    HOT_MENU, HOT_QUICK_SAVE, HOT_QUICK_LOAD, HOT_NEXT_SLOT, HOT_PREV_SLOT, HOT_SCREENSHOT,
    HOT_REWIND, HOT_PAUSE, HOT_SERVICE, HOT_TEST, HOT_FULLSCREEN, HOT_COUNT,
};

#define NSL_PLAYERS 3
#define NSL_SLOTS 8

typedef struct Settings {
    /* display */
    int fullscreen;
    int scale_mode;
    int filter_linear;
    int vsync;
    int rotation;                 /* 0, 90, 180, 270 */
    int scanlines;                /* 0..100 */
    int mask_type;
    int mask_strength;            /* 0..100 */
    int brightness;               /* 50..150 (%) */
    int gamma;                    /* 50..200 (x0.01) */
    int border;                   /* 0 black, 1 dark gradient, 2 accent gradient, 3 grid */
    int accent;                   /* index into the accent palette */
    int window_scale;             /* initial window size multiplier */
    /* audio */
    int volume;                   /* 0..100 */
    int mute;
    int latency_ms;               /* 32..200 */
    char audio_device[128];       /* "" = system default */
    /* controls */
    int key[NSL_PLAYERS][ACT_PLAYER_COUNT];       /* SDL_Scancode */
    int pad_button[NSL_PLAYERS][ACT_PLAYER_COUNT];/* SDL_GameControllerButton or -1 */
    int pad_index[NSL_PLAYERS];   /* which connected controller drives the player (-1 none) */
    int hot_key[HOT_COUNT];
    int hot_pad[HOT_COUNT];       /* controller button, -1 none */
    int menu_combo;               /* 0 Back+Start, 1 Guide, 2 both */
    int deadzone;                 /* 0..90 (%) */
    int analog_stick;             /* left stick drives the joystick */
    /* gameplay / QoL */
    int autosave;                 /* save to the auto slot on exit / return to menu */
    int rewind_seconds;           /* 0 = off */
    int confirm_overwrite;
    int slot;                     /* current quick slot 0..NSL_SLOTS-1 */
    int show_fps, show_frametime, show_inputs;
    int gadget_x[3], gadget_y[3], gadget_scale[3];
    /* ROM */
    char rom_path[1024];
    char browse_dir[1024];
} Settings;

extern Settings g_settings;

void settings_defaults(Settings *s);
void settings_default_controls(Settings *s, int preset);   /* 0 standard, 1 arcade stick */
int settings_load(Settings *s, const char *path);
int settings_save(const Settings *s, const char *path);
const char *action_name(int action);
const char *hotkey_name(int hotkey);

#endif
