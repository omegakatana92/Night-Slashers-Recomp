# Night Slashers Recomp — user guide

You need your own **`nslasher.zip`** (MAME set *Night Slashers (Korea Ver. 1.3, DE-0397-0
PCB)*). No ROM data is included in this program or its packages.

## First start and ROM selection

The program looks for the ROM in this order:

1. `--rom <path>` (or a path given as the only argument)
2. the location you picked last time (saved in `settings.ini`)
3. `nslasher.zip` next to the AppImage file (Linux)
4. `rom/nslasher.zip` or `nslasher.zip` next to the executable, then the same in the data folder

If none is found, the main menu shows **Select ROM...**. The ROM picker browses folders with
keyboard, mouse or controller (Windows also lists drives) and accepts:

- a `.zip` archive (files may be in a sub-folder inside the zip), or
- a folder containing the extracted files: choose **[ Use this folder ]**.

Every file is checked by size and CRC32. Errors list the missing files, files with the wrong
CRC (bad dump or another revision) and recognise the Japan set (`nslasherj`), which this build
cannot run because it was translated from the Korea program. Paths with spaces and non-ASCII
characters work on both Windows and Linux.

## Default controls

| Action | Player 1 keys | Player 2 keys | Controller (standard preset) |
| --- | --- | --- | --- |
| Move | Arrow keys | I J K L | D-pad or left stick |
| Attack (B1) | Z | A | A / Cross |
| Jump (B2) | X | S | B / Circle |
| Special (B3) | C | D | X / Square |
| Start | 1 | 2 | Menu / Start |
| Coin | 5 | 6 | View / Select (sent when released on its own) |

Player 3 has no default keys; assign them in **Controller Settings → Player 3 buttons**.
The **arcade stick preset** puts Attack/Jump/Special on X / A / Y.

| Hotkey | Keyboard | Controller |
| --- | --- | --- |
| Quick menu (pause) | Esc | View + Menu, or Guide (configurable) |
| Quick save / quick load | F5 / F8 | hold View + RB / hold View + LB |
| Previous / next quick slot | F6 / F7 | — |
| Rewind (hold, when enabled) | Backspace | L3 |
| Screenshot | F12 | — |
| Pause | P | — |
| Service coin / test mode | 9 / F2 | — |
| Fullscreen | F11 | — |

Everything can be rebound (Enter/A to rebind, Delete/Y to clear) and restored to defaults.
The board's own settings (difficulty, lives, coinage) are in the game's test mode (F2).

Steam Deck: the built-in controls appear as a standard controller. View + Menu opens the
quick menu; Steam / ... buttons stay with Steam.

## Save states

- 8 slots plus an **Autosave** slot. Each shows its date and a preview of the screen.
- **Quick menu → Save State** picks a slot; overwriting asks for confirmation (can be turned
  off in Gameplay Settings). F5/F8 save/load the quick slot without asking.
- **Autosave** (on by default): leaving the game through *Return to Main Menu* or *Exit*
  saves to the auto slot; **Continue** on the main menu resumes it.
- A state holds the complete board: translated-CPU registers and banked registers, work RAM,
  tile chips, rowscroll, palette and ACE effects, sprite RAM and buffers, DECO104 protection
  chip, EEPROM serial state, interrupt/VBLANK timing, the Z80, YM2151 (timers, envelopes),
  both OKI sample chips and the picture on screen. Loading restores at a frame boundary,
  clears queued audio and ignores held buttons until you release them.
- Each state records the game, the translated program's CRC, a fingerprint of all ROM data,
  the format version and the runtime's structure sizes. States from another revision, other
  ROM data or an incompatible build, and damaged or truncated files, are refused with a
  message and the running game is left untouched.
- States are separate from the game's EEPROM (its own settings and high scores), which is
  saved automatically to `nvram/nslasher.eeprom`.

## Display

Fit 4:3 (default) or Integer (pixel-perfect whole multiples); nearest or bilinear filtering;
fullscreen (desktop resolution) or window; VSync; rotation 0/90/180/270; scanlines; aperture
grille or slot mask with strength; brightness and gamma; border styles; menu accent colour.
The game always runs at the board's 57.80 Hz regardless of the display's refresh rate.
Curvature and widescreen are not offered (see Known issues).

## Audio

Master volume, mute, output device and latency (32–192 ms). The output keeps the queue at the
chosen latency with a small rate correction instead of letting it grow. The board mixes music
and effects in hardware, so there is no separate music/effects control.

## Gadgets and extras

- FPS counter, emulation frame time and an input display, each with position and size.
- Rewind (off, 10, 20, 30 or 60 s; about 1 MB per second of memory, bounded).
- Input recording: *Quick menu → Record Inputs*, then *Stop Recording*; *Extras → Play last
  recording* replays it exactly from its starting state.
- Screenshots (F12) are 320×240 PNGs of the game picture.

## Where files are kept

| Platform | Folder |
| --- | --- |
| Windows | `%APPDATA%\NightSlashersRecomp` |
| Linux / Steam Deck | `$XDG_DATA_HOME/NightSlashersRecomp` or `~/.local/share/NightSlashersRecomp` |

Override with the environment variable `NSL_DATA_DIR`. Contents: `settings.ini`,
`nvram/nslasher.eeprom`, `states/nslasher/slotN.nss` and `auto.nss`, `screenshots/`,
`recordings/`, `nightslashers.log`, and `missed_blocks.txt` (code addresses the fallback
executed; useful for developers). Nothing is written inside the AppImage.

## Steam Deck

1. Copy `NightSlashers-x86_64.AppImage` and `nslasher.zip` to the same folder (for example
   `~/Games/NightSlashers`), then `chmod +x NightSlashers-x86_64.AppImage` (or Properties →
   Permissions → executable).
2. Desktop Mode: double-click it. Gaming Mode: Steam → *Add a Non-Steam Game* → browse to
   the AppImage. Fullscreen is automatic in Gaming Mode.
3. No Proton is needed; this is a native Linux build.

## Command line

```
nightslashers [--rom PATH] [--fullscreen | --windowed | --window WxH] [--start]
```
