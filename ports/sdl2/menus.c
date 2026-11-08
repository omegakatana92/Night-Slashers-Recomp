/*
 * Night Slashers front end: menus (main menu, in-game quick menu, settings, ROM picker,
 * save-state slots, controls, gadgets, extras, about).
 *
 * Original layout and artwork: dark gradient backdrop, an accent bar and a sliding
 * focus highlight. Every item maps to real functionality in main.c / the core.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app.h"
#include "ui.h"
#include "nsl/platform.h"
#include "nsl/rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
    P_NONE, P_MAIN, P_QUICK, P_SLOTS, P_ROM, P_DISPLAY, P_AUDIO, P_CONTROLS, P_BINDINGS,
    P_HOTKEYS, P_GAMEPLAY, P_GADGETS, P_EXTRAS, P_ABOUT,
};
enum { K_ACTION, K_VALUE, K_LABEL };
enum { A_ACCEPT, A_LEFT, A_RIGHT, A_DELETE };

typedef struct Item {
    char label[96];
    char value[128];
    char desc[200];
    int kind;
    int id;
} Item;

typedef struct Page {
    int page;
    int focus;
    int scroll;
    int arg;                       /* slot page: 0 load, 1 save; bindings: player */
} Page;

static Page g_stack[16];
static int g_depth;
static Item g_items[128];
static int g_count;
static float g_focus_y;
static uint64_t g_page_time;

/* confirm dialog */
static struct { int active; char text[200]; int action; int arg; int choice; } g_confirm;
enum { C_RESTART, C_MAIN_MENU, C_EXIT, C_OVERWRITE, C_RESET_CONTROLS, C_RESET_ALL, C_START_NEW };

/* binding capture */
static struct { int active; int player; int action; int hotkey; int pad; uint64_t started; } g_capture;

/* ROM browser */
typedef struct Entry { char name[256]; int is_dir; int is_special; } Entry;
static Entry *g_entries;
static int g_entry_count, g_entry_capacity;
static char g_browse[1024];
static char g_rom_message[1024];

/* slot preview */
static SDL_Texture *g_preview;
static int g_preview_slot = -99, g_preview_valid;
static uint32_t g_preview_pixels[320 * 240];
static char g_slot_info[NSL_SLOTS + 1][160];

static int scale(void) { int s = g_app.height / 270; return s < 2 ? 2 : s; }
static UiColor accent(void) { return ui_accents[g_settings.accent & 7]; }

static void save_settings_now(void) {
    char path[1100];
    nsl_path_join(path, sizeof(path), g_app.data_dir, "settings.ini");
    settings_save(&g_settings, path);
}

/* ---- stack ------------------------------------------------------------------- */

static void push(int page, int arg) {
    if (g_depth >= 15) return;
    g_stack[g_depth].page = page;
    g_stack[g_depth].focus = 0;
    g_stack[g_depth].scroll = 0;
    g_stack[g_depth].arg = arg;
    g_depth++;
    g_page_time = app_now_ms();
    g_preview_slot = -99;
}

static void pop(void) {
    if (g_depth > 0) g_depth--;
    g_page_time = app_now_ms();
    g_preview_slot = -99;
    save_settings_now();
    if (g_depth == 0 && g_app.mode == MODE_GAME) {
        /* leaving the quick menu: resume without the closing button reaching the game */
        g_app.suppress_game_input = 1;
        g_app.next_frame = SDL_GetPerformanceCounter();
    }
}

int menus_active(void) { return g_depth > 0; }

void menus_open_main(void) {
    g_depth = 0;
    push(P_MAIN, 0);
}

void menus_open_quick(void) {
    g_depth = 0;
    push(P_QUICK, 0);
    if (g_app.audio) SDL_ClearQueuedAudio(g_app.audio);
}

void menus_init(void) {
    g_depth = 0;
    snprintf(g_browse, sizeof(g_browse), "%s", g_settings.browse_dir);
}

/* ---- item builders ------------------------------------------------------------- */

static Item *add(int kind, int id, const char *label, const char *desc) {
    if (g_count >= 128) return &g_items[127];
    Item *it = &g_items[g_count++];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    it->id = id;
    snprintf(it->label, sizeof(it->label), "%s", label);
    if (desc) snprintf(it->desc, sizeof(it->desc), "%s", desc);
    return it;
}

static void value(Item *it, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(it->value, sizeof(it->value), format, args);
    va_end(args);
}

static const char *onoff(int v) { return v ? "On" : "Off"; }

const char *pad_button_name(int button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return "A / Cross";
    case SDL_CONTROLLER_BUTTON_B: return "B / Circle";
    case SDL_CONTROLLER_BUTTON_X: return "X / Square";
    case SDL_CONTROLLER_BUTTON_Y: return "Y / Triangle";
    case SDL_CONTROLLER_BUTTON_BACK: return "View / Select";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide";
    case SDL_CONTROLLER_BUTTON_START: return "Menu / Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB / L1";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB / R1";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad Down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad Right";
    case -1: return "-";
    default: return "Button";
    }
}

static const char *key_name(int scancode) {
    if (scancode <= 0) return "-";
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return name && name[0] ? name : "?";
}

/* ---- ROM browser --------------------------------------------------------------- */

static void entry_add(const char *name, int is_dir, int special) {
    if (g_entry_count >= g_entry_capacity) {
        int capacity = g_entry_capacity ? g_entry_capacity * 2 : 256;
        Entry *grown = (Entry *)realloc(g_entries, (size_t)capacity * sizeof(Entry));
        if (!grown) return;
        g_entries = grown;
        g_entry_capacity = capacity;
    }
    Entry *e = &g_entries[g_entry_count++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->is_dir = is_dir;
    e->is_special = special;
}

static int ends_zip(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return 0;
    const char *ext = name + n - 4;
    return (ext[0] == '.') && (ext[1] == 'z' || ext[1] == 'Z') && (ext[2] == 'i' || ext[2] == 'I') && (ext[3] == 'p' || ext[3] == 'P');
}

static void list_callback(void *user, const char *name, int is_dir) {
    (void)user;
    if (name[0] == '.') return;                       /* hidden files */
    if (is_dir || ends_zip(name)) entry_add(name, is_dir, 0);
}

static int entry_compare(const void *a, const void *b) {
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    if (x->is_special != y->is_special) return y->is_special - x->is_special;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return SDL_strcasecmp(x->name, y->name);
}

static void browse_home(void) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    snprintf(g_browse, sizeof(g_browse), "%s", home ? home : ".");
}

static void browse_refresh(void) {
    g_entry_count = 0;
    if (!g_browse[0]) browse_home();
    if (!g_browse[0] || !strcmp(g_browse, "::drives")) {
#ifdef _WIN32
        DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; d++) {
            if (drives & (1u << d)) { char name[8]; snprintf(name, sizeof(name), "%c:\\", 'A' + d); entry_add(name, 1, 2); }
        }
        return;
#else
        snprintf(g_browse, sizeof(g_browse), "/");
#endif
    }
    entry_add("[ Use this folder (extracted ROM files) ]", 0, 3);
    entry_add("..", 1, 1);
    if (nsl_list_directory(g_browse, list_callback, NULL) < 0) {
        snprintf(g_rom_message, sizeof(g_rom_message), "Cannot open this folder.");
    }
    if (g_entry_count > 2) qsort(g_entries + 2, (size_t)(g_entry_count - 2), sizeof(Entry), entry_compare);
}

static void browse_up(void) {
    size_t n = strlen(g_browse);
    while (n > 1 && (g_browse[n - 1] == '/' || g_browse[n - 1] == '\\')) g_browse[--n] = 0;
    char *slash = strrchr(g_browse, '/');
    char *back = strrchr(g_browse, '\\');
    if (back > slash) slash = back;
#ifdef _WIN32
    if (n <= 3 && g_browse[1] == ':') { snprintf(g_browse, sizeof(g_browse), "::drives"); return; }
#endif
    if (slash == g_browse) g_browse[1] = 0;
    else if (slash) {
        *slash = 0;
#ifdef _WIN32
        if (strlen(g_browse) == 2 && g_browse[1] == ':') strcat(g_browse, "\\");
#endif
    }
}

static void rom_try(const char *path) {
    char summary[1024];
    if (nsl_rom_identify(path, summary, sizeof(summary)) != 0) {
        snprintf(g_rom_message, sizeof(g_rom_message), "%s", summary);
        return;
    }
    if (app_load_rom(path) == 0) {
        snprintf(g_rom_message, sizeof(g_rom_message), "Loaded: %s", nsl_core_rom_title());
        snprintf(g_settings.browse_dir, sizeof(g_settings.browse_dir), "%s", g_browse);
        app_toast("ROM ready");
        pop();
    } else {
        snprintf(g_rom_message, sizeof(g_rom_message), "%s", g_app.rom_status);
    }
}

/* ---- page builders --------------------------------------------------------------- */

static const char *page_title(int page) {
    switch (page) {
    case P_MAIN: return "NIGHT SLASHERS";
    case P_QUICK: return "PAUSED";
    case P_SLOTS: return g_stack[g_depth - 1].arg ? "SAVE STATE" : "LOAD STATE";
    case P_ROM: return "ROM SELECTION";
    case P_DISPLAY: return "DISPLAY";
    case P_AUDIO: return "AUDIO";
    case P_CONTROLS: return "CONTROLS";
    case P_BINDINGS: return "BUTTON MAPPING";
    case P_HOTKEYS: return "HOTKEYS";
    case P_GAMEPLAY: return "GAMEPLAY";
    case P_GADGETS: return "GADGETS";
    case P_EXTRAS: return "EXTRAS";
    case P_ABOUT: return "ABOUT";
    default: return "";
    }
}

static void build(const Page *p) {
    g_count = 0;
    Item *it;
    switch (p->page) {
    case P_MAIN:
        if (g_app.rom_ok) {
            add(K_ACTION, 1, "Start Game", "Power on the board and play from the beginning.");
            it = add(K_ACTION, 2, "Continue", "Resume from the autosave made when you last left the game.");
            if (!app_state_exists(-1)) value(it, "no autosave");
            add(K_ACTION, 3, "Load State", "Pick a save-state slot to resume from.");
        } else {
            it = add(K_LABEL, 0, "No ROM loaded", NULL);
            add(K_ACTION, 4, "Select ROM...", "Choose your nslasher.zip (Korea Ver. 1.3) to begin.");
        }
        if (g_app.rom_ok) add(K_ACTION, 4, "ROM Selection", "Choose or re-check the ROM archive or folder.");
        add(K_ACTION, 5, "Display Settings", "Scaling, filtering, CRT effects, borders and fullscreen.");
        add(K_ACTION, 6, "Audio Settings", "Volume, output device and latency.");
        add(K_ACTION, 7, "Controller Settings", "Keyboard and controller mapping for players 1-3.");
        add(K_ACTION, 8, "Gameplay Settings", "Autosave, rewind and save-state behaviour.");
        add(K_ACTION, 9, "Gadgets", "On-screen information panels.");
        add(K_ACTION, 10, "Extras", "Input recording, screenshots and folders.");
        add(K_ACTION, 11, "About", "Version, credits and licences.");
        add(K_ACTION, 12, "Exit", "Close the program.");
        break;
    case P_QUICK:
        add(K_ACTION, 1, "Resume", "Back to the game.");
        add(K_ACTION, 2, "Restart Game", "Reset the board. Unsaved progress is lost.");
        it = add(K_ACTION, 3, "Save State", "Save to a slot (with preview and timestamp).");
        value(it, "quick slot %d", g_settings.slot + 1);
        add(K_ACTION, 4, "Load State", "Load from a slot.");
        add(K_ACTION, 5, "Display Settings", NULL);
        add(K_ACTION, 6, "Audio Settings", NULL);
        add(K_ACTION, 7, "Controller Configuration", NULL);
        add(K_ACTION, 8, "Gadgets", NULL);
        it = add(K_ACTION, 9, g_app.recording ? "Stop Recording" : "Record Inputs", "Record this session's inputs for exact replay.");
        add(K_ACTION, 10, "Return to Main Menu", g_settings.autosave ? "Autosaves, then returns to the main menu." : "Ends the session (autosave is off).");
        add(K_ACTION, 11, "Exit", g_settings.autosave ? "Autosaves and closes the program." : "Closes the program (autosave is off).");
        break;
    case P_SLOTS:
        if (!p->arg) {
            it = add(K_ACTION, 100, "Autosave", "Written when you leave the game with autosave on.");
            value(it, "%s", g_slot_info[NSL_SLOTS]);
        }
        for (int s = 0; s < NSL_SLOTS; s++) {
            char label[32];
            snprintf(label, sizeof(label), "Slot %d%s", s + 1, s == g_settings.slot ? "  *" : "");
            it = add(K_ACTION, s, label, p->arg ? "Save here. * marks the quick-save slot (F5/F8)." : "Load this state. Delete / Y removes it.");
            value(it, "%s", g_slot_info[s]);
        }
        break;
    case P_ROM: {
        it = add(K_LABEL, 0, "Current", NULL);
        value(it, "%s", g_app.rom_ok ? g_settings.rom_path : "none");
        it = add(K_LABEL, 0, "Folder", NULL);
        value(it, "%s", strcmp(g_browse, "::drives") ? g_browse : "Drives");
        add(K_ACTION, 900, "[ Home folder ]", NULL);
        add(K_ACTION, 901, "[ Program folder ]", NULL);
        add(K_ACTION, 902, "[ Data folder ]", NULL);
#ifdef _WIN32
        add(K_ACTION, 903, "[ Drives ]", NULL);
#endif
        for (int i = 0; i < g_entry_count; i++) {
            char label[300];
            if (g_entries[i].is_dir && !g_entries[i].is_special) snprintf(label, sizeof(label), "%s/", g_entries[i].name);
            else snprintf(label, sizeof(label), "%s", g_entries[i].name);
            it = add(K_ACTION, 1000 + i, label,
                     g_entries[i].is_special == 3 ? "Use a folder that holds the extracted nslasher files."
                     : g_entries[i].is_dir ? "Open folder." : "Check and load this archive.");
            if (!g_entries[i].is_dir && !g_entries[i].is_special) value(it, "zip");
        }
        break;
    }
    case P_DISPLAY:
        it = add(K_VALUE, 1, "Mode", "Fullscreen uses the desktop resolution (Gamescope-friendly)."); value(it, g_settings.fullscreen ? "Fullscreen" : "Windowed");
        it = add(K_VALUE, 2, "Scaling", "Fit keeps 4:3 at the largest size; Integer uses whole multiples for pixel-perfect output."); value(it, g_settings.scale_mode == SCALE_INTEGER ? "Integer (pixel-perfect)" : "Fit 4:3");
        it = add(K_VALUE, 3, "Filtering", NULL); value(it, g_settings.filter_linear ? "Bilinear" : "Nearest");
        it = add(K_VALUE, 4, "VSync", "Presentation only: the game always runs at the board's 57.80 Hz."); value(it, "%s", onoff(g_settings.vsync));
        it = add(K_VALUE, 5, "Rotation", "Original orientation is 0 degrees."); value(it, "%d deg", g_settings.rotation);
        it = add(K_VALUE, 6, "Scanlines", NULL); value(it, "%d%%", g_settings.scanlines);
        it = add(K_VALUE, 7, "CRT mask", "Phosphor mask overlay."); value(it, g_settings.mask_type == MASK_NONE ? "Off" : g_settings.mask_type == MASK_APERTURE ? "Aperture grille" : "Slot mask");
        it = add(K_VALUE, 8, "Mask strength", NULL); value(it, "%d%%", g_settings.mask_strength);
        it = add(K_VALUE, 9, "Brightness", NULL); value(it, "%d%%", g_settings.brightness);
        it = add(K_VALUE, 10, "Gamma", NULL); value(it, "%.2f", g_settings.gamma / 100.0);
        it = add(K_VALUE, 11, "Border", "Backdrop around the 4:3 picture."); { static const char *b[4] = { "Black", "Dark gradient", "Accent gradient", "Grid" }; value(it, "%s", b[g_settings.border & 3]); }
        it = add(K_VALUE, 12, "Menu accent", NULL); value(it, "%s", ui_accent_names[g_settings.accent & 7]);
        add(K_LABEL, 0, "Curvature / widescreen: not supported (see docs)", NULL);
        add(K_ACTION, 20, "Restore display defaults", NULL);
        break;
    case P_AUDIO: {
        it = add(K_VALUE, 1, "Master volume", NULL); value(it, "%d%%", g_settings.volume);
        it = add(K_VALUE, 2, "Mute", NULL); value(it, "%s", onoff(g_settings.mute));
        it = add(K_VALUE, 3, "Latency", "Target queue length. Lower is more responsive; raise it only if you hear crackling."); value(it, "%d ms", g_settings.latency_ms);
        it = add(K_VALUE, 4, "Output device", "Applies immediately.");
        value(it, "%s", g_settings.audio_device[0] ? g_settings.audio_device : "System default");
        add(K_LABEL, 0, "Separate music/effects volume: not exposed by the board", NULL);
        break;
    }
    case P_CONTROLS: {
        for (int pl = 0; pl < NSL_PLAYERS; pl++) {
            char label[48];
            snprintf(label, sizeof(label), "Player %d controller", pl + 1);
            it = add(K_VALUE, 10 + pl, label, "Which connected controller drives this player.");
            int idx = g_settings.pad_index[pl];
            if (idx < 0) value(it, "None");
            else if (idx < g_app.pad_count) value(it, "%d: %s", idx + 1, SDL_GameControllerName(g_app.pads[idx]) ? SDL_GameControllerName(g_app.pads[idx]) : "controller");
            else value(it, "%d: (not connected)", idx + 1);
        }
        for (int pl = 0; pl < NSL_PLAYERS; pl++) {
            char label[48];
            snprintf(label, sizeof(label), "Player %d buttons...", pl + 1);
            add(K_ACTION, 20 + pl, label, "Remap keyboard keys and controller buttons.");
        }
        add(K_ACTION, 30, "Hotkeys...", "Menu, quick save/load, screenshot, rewind, service and test.");
        it = add(K_VALUE, 31, "Quick menu on controller", NULL);
        { static const char *c[3] = { "View + Menu", "Guide", "View + Menu or Guide" }; value(it, "%s", c[g_settings.menu_combo % 3]); }
        it = add(K_VALUE, 32, "Left stick moves", NULL); value(it, "%s", onoff(g_settings.analog_stick));
        it = add(K_VALUE, 33, "Stick dead zone", NULL); value(it, "%d%%", g_settings.deadzone);
        add(K_ACTION, 34, "Preset: standard pad", "A attack, B jump, X special.");
        add(K_ACTION, 35, "Preset: arcade stick", "X attack, A jump, Y special (Neo Geo-style row).");
        add(K_ACTION, 36, "Restore default controls", NULL);
        it = add(K_LABEL, 0, "Connected controllers", NULL); value(it, "%d", g_app.pad_count);
        break;
    }
    case P_BINDINGS: {
        int pl = p->arg;
        for (int a = 0; a < ACT_PLAYER_COUNT; a++) {
            char label[64];
            snprintf(label, sizeof(label), "%s - keyboard", action_name(a));
            it = add(K_ACTION, a, label, "Press Enter/A, then the new key. Delete/Y clears.");
            value(it, "%s", key_name(g_settings.key[pl][a]));
            snprintf(label, sizeof(label), "%s - controller", action_name(a));
            it = add(K_ACTION, 100 + a, label, a == ACT_COIN ? "Coin is sent when this button is released on its own." : "Press Enter/A, then the new button. Delete/Y clears.");
            value(it, "%s", pad_button_name(g_settings.pad_button[pl][a]));
        }
        break;
    }
    case P_HOTKEYS:
        for (int h = 0; h < HOT_COUNT; h++) {
            char label[64];
            snprintf(label, sizeof(label), "%s - keyboard", hotkey_name(h));
            it = add(K_ACTION, h, label, NULL);
            value(it, "%s", key_name(g_settings.hot_key[h]));
            if (h == HOT_MENU) continue;          /* controller menu uses the combo setting */
            snprintf(label, sizeof(label), "%s - controller", hotkey_name(h));
            it = add(K_ACTION, 100 + h, label, (h == HOT_QUICK_SAVE || h == HOT_QUICK_LOAD) ? "Hold View/Select while pressing it, so it cannot fire by accident." : NULL);
            value(it, "%s", pad_button_name(g_settings.hot_pad[h]));
        }
        break;
    case P_GAMEPLAY:
        it = add(K_VALUE, 1, "Autosave on exit", "Saves to the auto slot when you leave the game; Continue resumes it."); value(it, "%s", onoff(g_settings.autosave));
        it = add(K_VALUE, 2, "Rewind buffer", "Hold Backspace / L3 to rewind. Uses bounded memory (about 1 MB per second)."); if (g_settings.rewind_seconds) value(it, "%d s", g_settings.rewind_seconds); else value(it, "Off");
        it = add(K_VALUE, 3, "Confirm overwrite", NULL); value(it, "%s", onoff(g_settings.confirm_overwrite));
        it = add(K_VALUE, 4, "Quick-save slot", NULL); value(it, "%d", g_settings.slot + 1);
        add(K_LABEL, 0, "Difficulty, lives and coinage: use the game's test mode (F2)", NULL);
        break;
    case P_GADGETS: {
        static const char *names[3] = { "FPS counter", "Frame time", "Input display" };
        int *on[3] = { &g_settings.show_fps, &g_settings.show_frametime, &g_settings.show_inputs };
        for (int g = 0; g < 3; g++) {
            char label[64];
            it = add(K_VALUE, g * 10 + 1, names[g], NULL); value(it, "%s", onoff(*on[g]));
            snprintf(label, sizeof(label), "  position X"); it = add(K_VALUE, g * 10 + 2, label, NULL); value(it, "%d%%", g_settings.gadget_x[g]);
            snprintf(label, sizeof(label), "  position Y"); it = add(K_VALUE, g * 10 + 3, label, NULL); value(it, "%d%%", g_settings.gadget_y[g]);
            snprintf(label, sizeof(label), "  size"); it = add(K_VALUE, g * 10 + 4, label, NULL); value(it, "%d", g_settings.gadget_scale[g]);
        }
        add(K_LABEL, 0, "Score/lives/stage panels: not yet verified (see docs)", NULL);
        break;
    }
    case P_EXTRAS:
        it = add(K_ACTION, 1, "Play last recording", "Replays the last input recording exactly.");
        add(K_ACTION, 2, "Open data folder", "Settings, saves, screenshots, recordings and logs.");
        it = add(K_LABEL, 0, "Data folder", NULL); value(it, "%s", g_app.data_dir);
        add(K_ACTION, 3, "Restore all settings to defaults", NULL);
        break;
    case P_ABOUT:
        add(K_LABEL, 0, "Night Slashers - native recompilation", NULL);
        it = add(K_LABEL, 0, "Version", NULL); value(it, "0.2.0");
        it = add(K_LABEL, 0, "ROM", NULL); value(it, "%s", g_app.rom_ok ? nsl_core_rom_title() : "none");
        add(K_LABEL, 0, "ARM program translated ahead of time to C", NULL);
        add(K_LABEL, 0, "Board, video, audio behaviour: MAME (reference)", NULL);
        add(K_LABEL, 0, "YM2151: ymfm (BSD-3)  Z80: superzazu/z80 (MIT)", NULL);
        add(K_LABEL, 0, "miniz (MIT)  font8x8 (public domain)  SDL2 (zlib)", NULL);
        add(K_LABEL, 0, "Night Slashers (c) 1994 Data East. Not affiliated.", NULL);
        add(K_LABEL, 0, "No ROM data is included in this program.", NULL);
        break;
    }
}

/* ---- slot info cache ---------------------------------------------------------- */

static void refresh_slot_info(void) {
    for (int s = 0; s < NSL_SLOTS; s++) app_state_info(s, g_slot_info[s], sizeof(g_slot_info[s]), NULL);
    app_state_info(-1, g_slot_info[NSL_SLOTS], sizeof(g_slot_info[NSL_SLOTS]), NULL);
}

static void update_preview(int slot) {
    if (slot == g_preview_slot) return;
    g_preview_slot = slot;
    char info[160];
    g_preview_valid = app_state_info(slot, info, sizeof(info), g_preview_pixels) != -1 && app_state_exists(slot);
    if (!g_preview) g_preview = SDL_CreateTexture(g_app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    if (g_preview && g_preview_valid) {
        for (int i = 0; i < 320 * 240; i++) g_preview_pixels[i] |= 0xFF000000u;
        SDL_UpdateTexture(g_preview, NULL, g_preview_pixels, 320 * 4);
    }
}

/* ---- actions ------------------------------------------------------------------ */

static void confirm(const char *text, int action, int arg) {
    g_confirm.active = 1;
    snprintf(g_confirm.text, sizeof(g_confirm.text), "%s", text);
    g_confirm.action = action;
    g_confirm.arg = arg;
    g_confirm.choice = 1;                          /* default to "No" */
}

static void run_confirmed(int action, int arg) {
    switch (action) {
    case C_RESTART: g_depth = 0; app_restart_game(); break;
    case C_MAIN_MENU: app_leave_game(1); break;
    case C_EXIT: app_quit(); break;
    case C_OVERWRITE: app_save_state(arg); refresh_slot_info(); g_preview_slot = -99; break;
    case C_RESET_CONTROLS: settings_default_controls(&g_settings, 0); app_toast("Controls restored"); break;
    case C_RESET_ALL: {
        char rom[1024];
        snprintf(rom, sizeof(rom), "%s", g_settings.rom_path);
        settings_defaults(&g_settings);
        snprintf(g_settings.rom_path, sizeof(g_settings.rom_path), "%s", rom);
        app_apply_display();
        app_reopen_audio();
        app_rewind_configure();
        app_toast("All settings restored");
        break;
    }
    case C_START_NEW: g_depth = 0; app_start_game(); break;
    }
    save_settings_now();
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void cycle_audio_device(int dir) {
    int count = SDL_GetNumAudioDevices(0);
    int current = -1;
    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        if (name && !strcmp(name, g_settings.audio_device)) current = i;
    }
    int next = current + dir;
    if (next < -1) next = count - 1;
    if (next >= count) next = -1;
    if (next < 0) g_settings.audio_device[0] = 0;
    else snprintf(g_settings.audio_device, sizeof(g_settings.audio_device), "%s", SDL_GetAudioDeviceName(next, 0));
    app_reopen_audio();
}

static void open_slots(int save_mode) {
    refresh_slot_info();
    push(P_SLOTS, save_mode);
    if (!save_mode) g_stack[g_depth - 1].focus = 1 + g_settings.slot;
    else g_stack[g_depth - 1].focus = g_settings.slot;
}

static void act(Page *p, Item *it, int action) {
    int d = action == A_LEFT ? -1 : action == A_RIGHT ? 1 : 1;
    int id = it->id;
    switch (p->page) {
    case P_MAIN:
        if (action != A_ACCEPT) return;
        switch (id) {
        case 1:
            if (g_app.game_active) confirm("A game is in progress. Start over?", C_START_NEW, 0);
            else { g_depth = 0; app_start_game(); }
            break;
        case 2: g_depth = 0; app_continue_game(); if (!g_app.game_active) menus_open_main(); break;
        case 3: open_slots(0); break;
        case 4: if (!g_browse[0]) snprintf(g_browse, sizeof(g_browse), "%s", g_settings.browse_dir); browse_refresh(); g_rom_message[0] = 0; push(P_ROM, 0); break;
        case 5: push(P_DISPLAY, 0); break;
        case 6: push(P_AUDIO, 0); break;
        case 7: push(P_CONTROLS, 0); break;
        case 8: push(P_GAMEPLAY, 0); break;
        case 9: push(P_GADGETS, 0); break;
        case 10: push(P_EXTRAS, 0); break;
        case 11: push(P_ABOUT, 0); break;
        case 12: app_quit(); break;
        }
        break;
    case P_QUICK:
        if (action != A_ACCEPT) return;
        switch (id) {
        case 1: pop(); break;
        case 2: confirm("Restart the game? Unsaved progress is lost.", C_RESTART, 0); break;
        case 3: open_slots(1); break;
        case 4: open_slots(0); break;
        case 5: push(P_DISPLAY, 0); break;
        case 6: push(P_AUDIO, 0); break;
        case 7: push(P_CONTROLS, 0); break;
        case 8: push(P_GADGETS, 0); break;
        case 9: if (g_app.recording) app_record_stop(); else { app_record_start(); pop(); } break;
        case 10:
            if (g_settings.autosave) app_leave_game(1);
            else confirm("Return to the main menu? Unsaved progress is lost.", C_MAIN_MENU, 0);
            break;
        case 11:
            if (g_settings.autosave) app_quit();
            else confirm("Exit? Unsaved progress is lost.", C_EXIT, 0);
            break;
        }
        break;
    case P_SLOTS: {
        int slot = id == 100 ? -1 : id;
        if (action == A_DELETE && slot >= 0 && app_state_exists(slot)) {
            char path[1200];
            app_state_path(slot, path, sizeof(path));
            nsl_remove_file(path);
            refresh_slot_info();
            g_preview_slot = -99;
            app_toast("Slot %d deleted", slot + 1);
            return;
        }
        if (action == A_LEFT || action == A_RIGHT) return;
        if (action != A_ACCEPT) return;
        if (p->arg) {                                   /* save */
            g_settings.slot = slot;
            if (app_state_exists(slot) && g_settings.confirm_overwrite) {
                char text[96];
                snprintf(text, sizeof(text), "Overwrite slot %d?", slot + 1);
                confirm(text, C_OVERWRITE, slot);
            } else {
                app_save_state(slot);
                refresh_slot_info();
                g_preview_slot = -99;
            }
        } else {                                        /* load */
            if (!app_state_exists(slot)) { app_toast("That slot is empty"); return; }
            if (slot >= 0) g_settings.slot = slot;
            int was_active = g_app.game_active;
            if (!was_active) { nsl_core_reset(); g_app.game_active = 1; }
            if (app_load_state(slot) == 0) {
                g_app.mode = MODE_GAME;
                g_depth = 0;
                g_app.suppress_game_input = 1;
            } else if (!was_active) {
                g_app.game_active = 0;
            }
        }
        break;
    }
    case P_ROM:
        if (action != A_ACCEPT) return;
        if (id == 900) { browse_home(); browse_refresh(); p->focus = 0; return; }
        if (id == 901) { snprintf(g_browse, sizeof(g_browse), "%s", g_app.exe_dir); browse_refresh(); p->focus = 0; return; }
        if (id == 902) { snprintf(g_browse, sizeof(g_browse), "%s", g_app.data_dir); browse_refresh(); p->focus = 0; return; }
        if (id == 903) { snprintf(g_browse, sizeof(g_browse), "::drives"); browse_refresh(); p->focus = 0; return; }
        if (id >= 1000 && id - 1000 < g_entry_count) {
            Entry *e = &g_entries[id - 1000];
            if (e->is_special == 3) { rom_try(g_browse); return; }
            if (e->is_special == 1) { browse_up(); browse_refresh(); p->focus = 0; return; }
            char path[1400];
            if (e->is_special == 2) snprintf(path, sizeof(path), "%s", e->name);
            else nsl_path_join(path, sizeof(path), g_browse, e->name);
            if (e->is_dir) { snprintf(g_browse, sizeof(g_browse), "%s", path); browse_refresh(); p->focus = 0; }
            else rom_try(path);
        }
        break;
    case P_DISPLAY:
        switch (id) {
        case 1: g_settings.fullscreen = !g_settings.fullscreen; app_apply_window_mode(); break;
        case 2: g_settings.scale_mode = !g_settings.scale_mode; break;
        case 3: g_settings.filter_linear = !g_settings.filter_linear; break;
        case 4: g_settings.vsync = !g_settings.vsync; app_apply_display(); break;
        case 5: g_settings.rotation = (g_settings.rotation + 360 + d * 90) % 360; break;
        case 6: g_settings.scanlines = clampi(g_settings.scanlines + d * 10, 0, 100); break;
        case 7: g_settings.mask_type = (g_settings.mask_type + 3 + d) % 3; break;
        case 8: g_settings.mask_strength = clampi(g_settings.mask_strength + d * 10, 0, 100); break;
        case 9: g_settings.brightness = clampi(g_settings.brightness + d * 5, 50, 150); break;
        case 10: g_settings.gamma = clampi(g_settings.gamma + d * 5, 50, 200); break;
        case 11: g_settings.border = (g_settings.border + 4 + d) % 4; break;
        case 12: g_settings.accent = (g_settings.accent + 8 + d) % 8; break;
        case 20:
            if (action == A_ACCEPT) {
                Settings defaults;
                settings_defaults(&defaults);
                g_settings.scale_mode = defaults.scale_mode; g_settings.filter_linear = defaults.filter_linear;
                g_settings.vsync = defaults.vsync; g_settings.rotation = 0; g_settings.scanlines = 0;
                g_settings.mask_type = MASK_NONE; g_settings.mask_strength = defaults.mask_strength;
                g_settings.brightness = 100; g_settings.gamma = 100; g_settings.border = defaults.border;
                app_apply_display();
                app_toast("Display settings restored");
            }
            break;
        }
        break;
    case P_AUDIO:
        switch (id) {
        case 1: g_settings.volume = clampi(g_settings.volume + d * 5, 0, 100); break;
        case 2: g_settings.mute = !g_settings.mute; break;
        case 3: g_settings.latency_ms = clampi(g_settings.latency_ms + d * 16, 32, 192); if (g_app.audio) SDL_ClearQueuedAudio(g_app.audio); break;
        case 4: cycle_audio_device(d); break;
        }
        break;
    case P_CONTROLS:
        if (id >= 10 && id < 10 + NSL_PLAYERS) {
            int pl = id - 10;
            g_settings.pad_index[pl] = clampi(g_settings.pad_index[pl] + d, -1, 7);
        } else if (action == A_ACCEPT) {
            if (id >= 20 && id < 20 + NSL_PLAYERS) push(P_BINDINGS, id - 20);
            else if (id == 30) push(P_HOTKEYS, 0);
            else if (id == 34) { settings_default_controls(&g_settings, 0); app_toast("Standard pad preset"); }
            else if (id == 35) { settings_default_controls(&g_settings, 1); app_toast("Arcade stick preset"); }
            else if (id == 36) confirm("Restore every control to its default?", C_RESET_CONTROLS, 0);
        }
        if (id == 31 && action != A_ACCEPT) g_settings.menu_combo = (g_settings.menu_combo + 3 + d) % 3;
        if (id == 31 && action == A_ACCEPT) g_settings.menu_combo = (g_settings.menu_combo + 1) % 3;
        if (id == 32) g_settings.analog_stick = !g_settings.analog_stick;
        if (id == 33) g_settings.deadzone = clampi(g_settings.deadzone + d * 5, 0, 90);
        break;
    case P_BINDINGS:
    case P_HOTKEYS: {
        int hot = p->page == P_HOTKEYS;
        int pad = id >= 100;
        int index = id % 100;
        if (action == A_DELETE) {
            if (hot) { if (pad) g_settings.hot_pad[index] = -1; else if (index != HOT_MENU) g_settings.hot_key[index] = 0; }
            else { if (pad) g_settings.pad_button[p->arg][index] = -1; else g_settings.key[p->arg][index] = 0; }
            return;
        }
        if (action != A_ACCEPT) return;
        g_capture.active = 1;
        g_capture.player = p->arg;
        g_capture.action = index;
        g_capture.hotkey = hot;
        g_capture.pad = pad;
        g_capture.started = app_now_ms();
        break;
    }
    case P_GAMEPLAY:
        switch (id) {
        case 1: g_settings.autosave = !g_settings.autosave; break;
        case 2: {
            static const int steps[5] = { 0, 10, 20, 30, 60 };
            int at = 0;
            for (int i = 0; i < 5; i++) if (steps[i] == g_settings.rewind_seconds) at = i;
            at = (at + 5 + d) % 5;
            g_settings.rewind_seconds = steps[at];
            app_rewind_configure();
            break;
        }
        case 3: g_settings.confirm_overwrite = !g_settings.confirm_overwrite; break;
        case 4: g_settings.slot = (g_settings.slot + NSL_SLOTS + d) % NSL_SLOTS; break;
        }
        break;
    case P_GADGETS: {
        int g = id / 10, field = id % 10;
        int *on[3] = { &g_settings.show_fps, &g_settings.show_frametime, &g_settings.show_inputs };
        if (field == 1) *on[g] = !*on[g];
        if (field == 2) g_settings.gadget_x[g] = clampi(g_settings.gadget_x[g] + d * 2, 0, 100);
        if (field == 3) g_settings.gadget_y[g] = clampi(g_settings.gadget_y[g] + d * 2, 0, 100);
        if (field == 4) g_settings.gadget_scale[g] = clampi(g_settings.gadget_scale[g] + d, 1, 4);
        break;
    }
    case P_EXTRAS:
        if (action != A_ACCEPT) return;
        if (id == 1) {
            if (!nsl_core_loaded()) { app_toast("Load a ROM first"); return; }
            if (!g_app.game_active) { nsl_core_reset(); }
            if (app_playback_start() == 0) g_depth = 0;
        }
        if (id == 2) {
            char url[1200];
#ifdef _WIN32
            snprintf(url, sizeof(url), "file:///%s", g_app.data_dir);
#else
            snprintf(url, sizeof(url), "file://%s", g_app.data_dir);
#endif
            if (SDL_OpenURL(url) != 0) app_toast("Data folder: %s", g_app.data_dir);
        }
        if (id == 3) confirm("Restore all settings to their defaults?", C_RESET_ALL, 0);
        break;
    }
}

/* ---- update -------------------------------------------------------------------- */

static int item_height(void) { return 16 * scale() / 2 + 12 * scale() / 2; }

static void list_geometry(int *x, int *y, int *w, int *visible) {
    int s = scale();
    int top = g_app.height / 7 + 16 * (s + 1) / 2;
    int bottom = g_app.height - g_app.height / 6;
    *visible = (bottom - top) / item_height();
    if (*visible < 3) *visible = 3;
    int wide = g_depth && (g_stack[g_depth - 1].page == P_SLOTS);
    *w = wide ? g_app.width * 50 / 100 : g_app.width * 70 / 100;
    if (*w > 1100 * s / 3) *w = 1100 * s / 3;
    *x = wide ? g_app.width / 16 : (g_app.width - *w) / 2;
    *y = top;
}

static int selectable(int i) { return g_items[i].kind != K_LABEL; }

static void capture_update(const MenuInput *in) {
    if (app_now_ms() - g_capture.started < 150) return;      /* ignore the press that started it */
    int pl = g_capture.player, a = g_capture.action;
    if (!g_capture.pad) {
        if (in->any_key_scancode < 0) return;
        if (in->any_key_scancode == SDL_SCANCODE_ESCAPE && !(g_capture.hotkey && a == HOT_MENU)) { g_capture.active = 0; return; }
        if (g_capture.hotkey) g_settings.hot_key[a] = in->any_key_scancode;
        else g_settings.key[pl][a] = in->any_key_scancode;
        g_capture.active = 0;
    } else {
        if (in->any_key_scancode == SDL_SCANCODE_ESCAPE) { g_capture.active = 0; return; }
        if (in->any_pad_button < 0) return;
        if (g_capture.hotkey) g_settings.hot_pad[a] = in->any_pad_button;
        else g_settings.pad_button[pl][a] = in->any_pad_button;
        g_capture.active = 0;
    }
    save_settings_now();
}

void menus_update(const MenuInput *in) {
    if (!g_depth) return;
    if (g_capture.active) { capture_update(in); return; }
    Page *p = &g_stack[g_depth - 1];
    build(p);

    if (g_confirm.active) {
        if (in->left || in->right || in->up || in->down) g_confirm.choice ^= 1;
        if (in->mouse_click) {
            int bw = g_app.width / 6, cx = g_app.width / 2;
            g_confirm.choice = in->mouse_x < cx ? 0 : 1;
            (void)bw;
        }
        if (in->accept || in->mouse_click) {
            g_confirm.active = 0;
            if (g_confirm.choice == 0) run_confirmed(g_confirm.action, g_confirm.arg);
        }
        if (in->back) g_confirm.active = 0;
        return;
    }

    if (g_count == 0) { if (in->back) pop(); return; }
    if (p->focus >= g_count) p->focus = g_count - 1;
    if (!selectable(p->focus)) {
        for (int i = 0; i < g_count; i++) if (selectable((p->focus + i) % g_count)) { p->focus = (p->focus + i) % g_count; break; }
    }
    int step = 0;
    if (in->up) step = -1;
    if (in->down) step = 1;
    if (in->mouse_wheel) step = in->mouse_wheel > 0 ? -1 : 1;
    if (in->page_up || in->page_down) {
        int lx, ly, lw, vis;
        list_geometry(&lx, &ly, &lw, &vis);
        for (int n = 0; n < vis; n++) {
            int next = p->focus + (in->page_up ? -1 : 1);
            if (next < 0 || next >= g_count) break;
            p->focus = next;
        }
        while (!selectable(p->focus) && p->focus < g_count - 1) p->focus++;
    }
    if (step) {
        int next = p->focus;
        for (int n = 0; n < g_count; n++) {
            next = (next + step + g_count) % g_count;
            if (selectable(next)) break;
        }
        p->focus = next;
    }
    int lx, ly, lw, vis;
    list_geometry(&lx, &ly, &lw, &vis);
    if (p->focus < p->scroll) p->scroll = p->focus;
    if (p->focus >= p->scroll + vis) p->scroll = p->focus - vis + 1;
    if (p->scroll < 0) p->scroll = 0;

    /* mouse hover / click */
    if (in->mouse_moved || in->mouse_click) {
        for (int row = 0; row < vis && p->scroll + row < g_count; row++) {
            int i = p->scroll + row;
            int y = ly + row * item_height();
            if (in->mouse_x >= lx && in->mouse_x < lx + lw && in->mouse_y >= y && in->mouse_y < y + item_height() && selectable(i)) {
                p->focus = i;
                if (in->mouse_click) {
                    Item *it = &g_items[i];
                    if (it->kind == K_VALUE) act(p, it, in->mouse_x < lx + lw * 2 / 3 ? A_RIGHT : (in->mouse_x < lx + lw * 5 / 6 ? A_LEFT : A_RIGHT));
                    else act(p, it, A_ACCEPT);
                    return;
                }
            }
        }
    }

    Item *it = &g_items[p->focus];
    if (in->accept && selectable(p->focus)) { act(p, it, it->kind == K_VALUE ? A_RIGHT : A_ACCEPT); return; }
    if (in->left && it->kind == K_VALUE) act(p, it, A_LEFT);
    if (in->right && it->kind == K_VALUE) act(p, it, A_RIGHT);
    if (in->delete_key) act(p, it, A_DELETE);
    if (in->back) {
        if (p->page == P_ROM) { browse_up(); browse_refresh(); p->focus = 0; if (in->back && g_browse[0] == 0) pop(); return; }
        if (p->page == P_MAIN) return;                    /* main menu: use Exit */
        pop();
    }
}

/* ---- draw ---------------------------------------------------------------------- */

static void draw_hints(const char *text) {
    int s = scale();
    UiColor dim = ui_rgba(170, 170, 180, 255);
    ui_fill(0, g_app.height - 16 * s / 2 - 24, g_app.width, 16 * s / 2 + 24, ui_rgba(0, 0, 0, 140));
    ui_text(24, g_app.height - 16 * s / 2 - 12, text, s, dim);
}

void menus_draw(void) {
    if (!g_depth) return;
    Page *p = &g_stack[g_depth - 1];
    build(p);
    int s = scale();
    float fade = (app_now_ms() - g_page_time) / 160.0f;
    if (fade > 1) fade = 1;
    Uint8 alpha = (Uint8)(255 * fade);

    if (!g_app.game_active) {
        UiColor top = ui_mix(ui_rgba(18, 18, 26, 255), accent(), 0.10f);
        ui_gradient(0, 0, g_app.width, g_app.height, top, ui_rgba(4, 4, 6, 255));
        /* original backdrop: diagonal slashes in the accent colour */
        for (int i = 0; i < 7; i++) {
            int x = g_app.width * (i * 2 + 9) / 20;
            for (int y = 0; y < g_app.height; y += 4) {
                int off = (g_app.height - y) / 3;
                ui_fill(x + off, y, 3 + i % 3, 4, ui_mix(ui_rgba(0, 0, 0, 0), accent(), 0.06f + 0.02f * (i % 3)));
            }
        }
    }

    /* header */
    int title_scale = s + 2;
    int header_y = g_app.height / 14;
    const char *title = page_title(p->page);
    ui_text_shadow(g_app.width / 16, header_y, title, title_scale, ui_rgba(245, 245, 250, alpha));
    ui_fill(g_app.width / 16, header_y + 16 * title_scale / 2 + 6, ui_text_width(title, title_scale), 4, accent());
    if (p->page == P_MAIN) {
        const char *sub = g_app.rom_ok ? "Native recompilation" : g_app.rom_status;
        ui_text(g_app.width / 16, header_y + 16 * title_scale / 2 + 18, sub, s - (s > 2), ui_rgba(160, 160, 170, alpha));
    }

    int lx, ly, lw, vis;
    list_geometry(&lx, &ly, &lw, &vis);
    int ih = item_height();
    ui_fill(lx - 12, ly - 12, lw + 24, vis * ih + 24 > (g_count - p->scroll) * ih + 24 ? (g_count - p->scroll) * ih + 24 : vis * ih + 24, ui_rgba(8, 8, 12, 150));

    /* sliding focus bar */
    float target = (float)(ly + (p->focus - p->scroll) * ih);
    g_focus_y += (target - g_focus_y) * 0.35f;
    if (g_focus_y < ly - ih || g_focus_y > ly + vis * ih) g_focus_y = target;
    ui_fill(lx - 8, (int)g_focus_y, lw + 16, ih, ui_mix(ui_rgba(30, 30, 40, 230), accent(), 0.35f));
    ui_fill(lx - 8, (int)g_focus_y, 5, ih, accent());

    for (int row = 0; row < vis && p->scroll + row < g_count; row++) {
        int i = p->scroll + row;
        Item *it = &g_items[i];
        int y = ly + row * ih + (ih - 16 * s / 2) / 2;
        int focused = i == p->focus;
        UiColor color = it->kind == K_LABEL ? ui_rgba(150, 150, 165, alpha) : focused ? ui_rgba(255, 255, 255, alpha) : ui_rgba(200, 200, 210, alpha);
        ui_text_shadow(lx + 6, y, it->label, s, color);
        if (it->value[0]) {
            char shown[128];
            snprintf(shown, sizeof(shown), it->kind == K_VALUE && focused ? "< %s >" : "%s", it->value);
            int max_chars = (lw / 2) / (ui_text_width("M", s) ? ui_text_width("M", s) : 1);
            if ((int)strlen(shown) > max_chars && max_chars > 4) {
                memmove(shown + 3, shown + strlen(shown) - (max_chars - 3), (size_t)(max_chars - 3) + 1);
                shown[0] = shown[1] = shown[2] = '.';
            }
            ui_text_right(lx + lw - 6, y, shown, s, focused ? accent() : ui_rgba(170, 170, 185, alpha));
        }
    }
    if (p->scroll > 0) ui_text_center(lx + lw / 2, ly - ih / 2 - 8, "^", s, accent());
    if (p->scroll + vis < g_count) ui_text_center(lx + lw / 2, ly + vis * ih + 4, "v", s, accent());

    /* description */
    Item *focus = &g_items[p->focus];
    int desc_y = g_app.height - g_app.height / 6 + 16;
    if (p->page == P_ROM && g_rom_message[0]) {
        ui_text(g_app.width / 16, desc_y, g_rom_message, s - (s > 2), ui_rgba(255, 210, 120, alpha));
    } else if (focus->desc[0]) {
        ui_text(g_app.width / 16, desc_y, focus->desc, s - (s > 2), ui_rgba(185, 185, 195, alpha));
    }

    /* slot preview */
    if (p->page == P_SLOTS) {
        int slot = focus->id == 100 ? -1 : focus->id;
        update_preview(slot);
        int pw = g_app.width * 36 / 100, ph = pw * 3 / 4;
        int px = g_app.width - pw - g_app.width / 16, py = ly;
        ui_fill(px - 6, py - 6, pw + 12, ph + 12, accent());
        if (g_preview_valid && g_preview) {
            SDL_Rect dst = { px, py, pw, ph };
            SDL_RenderCopy(g_app.renderer, g_preview, NULL, &dst);
        } else {
            ui_fill(px, py, pw, ph, ui_rgba(10, 10, 14, 255));
            ui_text_center(px + pw / 2, py + ph / 2 - 8, "EMPTY", s + 1, ui_rgba(90, 90, 100, 255));
        }
        ui_text(px, py + ph + 14, focus->value, s, ui_rgba(220, 220, 230, alpha));
    }

    if (p->page == P_MAIN) draw_hints("Enter/A select   Arrows/D-pad move   Mouse supported");
    else if (p->page == P_SLOTS) draw_hints("A select   B back   Y/Delete erase slot   LB/RB page");
    else if (p->page == P_BINDINGS || p->page == P_HOTKEYS) draw_hints("A remap   Y/Delete clear   B back");
    else draw_hints("A select   </> change   B/Esc back");

    if (g_capture.active) {
        ui_fill(0, 0, g_app.width, g_app.height, ui_rgba(0, 0, 0, 200));
        ui_text_center(g_app.width / 2, g_app.height / 2 - 30, g_capture.pad ? "Press a controller button" : "Press a key", s + 1, ui_rgba(255, 255, 255, 255));
        ui_text_center(g_app.width / 2, g_app.height / 2 + 20, "Esc cancels", s, ui_rgba(170, 170, 180, 255));
    }
    if (g_confirm.active) {
        int w = g_app.width / 2 + 80, h = g_app.height / 4;
        int x = (g_app.width - w) / 2, y = (g_app.height - h) / 2;
        ui_fill(0, 0, g_app.width, g_app.height, ui_rgba(0, 0, 0, 150));
        ui_fill(x, y, w, h, ui_rgba(16, 16, 22, 250));
        ui_frame(x, y, w, h, 3, accent());
        ui_text_center(g_app.width / 2, y + h / 4, g_confirm.text, s, ui_rgba(240, 240, 245, 255));
        const char *labels[2] = { "Yes", "No" };
        for (int b = 0; b < 2; b++) {
            int bx = g_app.width / 2 + (b ? 40 : -40 - g_app.width / 8), by = y + h * 3 / 5, bw = g_app.width / 8, bh = 16 * s / 2 + 16;
            ui_fill(bx, by, bw, bh, b == g_confirm.choice ? accent() : ui_rgba(40, 40, 50, 255));
            ui_text_center(bx + bw / 2, by + 8, labels[b], s, ui_rgba(255, 255, 255, 255));
        }
    }
}
