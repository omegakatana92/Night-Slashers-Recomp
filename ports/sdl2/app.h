/*
 * Night Slashers front end: shared application state.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NSL_FE_APP_H
#define NSL_FE_APP_H

#include <SDL.h>
#include <stdint.h>

#include "config.h"
#include "nsl/core.h"

#define MAX_PADS 8

enum { MODE_MENU, MODE_GAME };

typedef struct MenuInput {
    int up, down, left, right, accept, back, page_up, page_down, delete_key;
    int mouse_x, mouse_y, mouse_moved, mouse_click, mouse_wheel;
    int any_key_scancode;           /* for binding capture, -1 none */
    int any_pad_button;             /* for binding capture, -1 none */
} MenuInput;

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *game_texture;
    SDL_Texture *scanline_texture;
    SDL_Texture *mask_texture;
    int mask_w, mask_h, mask_kind, mask_strength, scan_strength;
    SDL_AudioDeviceID audio;
    int audio_rate;
    int width, height;
    int mode;
    int running;
    int game_active;               /* a session exists (running or paused under a menu) */
    int quick_menu;                /* menu opened from inside the game */
    int paused;
    char data_dir[1024];
    char state_dir[1024];
    char log_path[1024];
    char exe_dir[1024];
    char rom_status[1024];
    int rom_ok;
    /* controllers */
    SDL_GameController *pads[MAX_PADS];
    SDL_JoystickID pad_ids[MAX_PADS];
    int pad_count;
    /* timing */
    double frame_period;
    uint64_t perf_freq;
    uint64_t next_frame;
    double fps, frame_ms, emu_ms;
    uint64_t fps_frames, fps_start;
    /* toasts */
    char toast[160];
    uint64_t toast_until;
    /* input */
    NslInputState input;
    int suppress_game_input;       /* until every bound control is released */
    int back_held_alone;           /* controller Back pressed with nothing else: coin on release */
    int coin_pulse[3];
    int service_pulse;
    /* rewind */
    uint8_t **rewind;
    int *rewind_size;
    int rewind_capacity, rewind_head, rewind_count;
    int rewinding;
    /* recording */
    int recording, playing_back;
    uint8_t *rec_state;
    int rec_state_size;
    NslInputState *rec_inputs;
    int rec_count, rec_capacity, rec_position;
    /* EEPROM */
    uint8_t eeprom_saved[128];
    uint64_t eeprom_check_frame;
} App;

extern App g_app;

void app_log(const char *format, ...);
void app_toast(const char *format, ...);
uint64_t app_now_ms(void);

/* game session */
int app_load_rom(const char *path);
void app_start_game(void);
void app_continue_game(void);
void app_restart_game(void);
void app_leave_game(int to_menu);      /* autosave, EEPROM flush */
void app_quit(void);
void app_save_eeprom(void);

/* states (slot -1 = auto) */
void app_state_path(int slot, char *out, size_t size);
int app_save_state(int slot);
int app_load_state(int slot);
int app_state_exists(int slot);
int app_state_info(int slot, char *text, size_t size, uint32_t *preview);
void app_screenshot(void);

/* recording */
void app_record_start(void);
void app_record_stop(void);
int app_playback_start(void);

/* display/audio (re)configuration */
void app_apply_display(void);
void app_apply_window_mode(void);
void app_reopen_audio(void);
void app_rewind_configure(void);

/* drawing */
void app_draw_game(int dim);
void app_draw_background(void);
void app_draw_gadgets(void);

/* menus (menus.c) */
void menus_init(void);
void menus_open_main(void);
void menus_open_quick(void);
int menus_active(void);
void menus_update(const MenuInput *input);
void menus_draw(void);
const char *pad_button_name(int button);

#endif
