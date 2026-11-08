# Changelog

## [0.2.0] - 2026-11-08 — first playable build

### Fixed
- Boot: shifter carry evaluation order, byte-address alignment, frame timing and EEPROM
  model (the "EEPROM blocker" was a translator bug).

### Added
- Runtime ROM loader for zip archives or folders with CRC checks and clear errors; DECO156
  and DECO56/74 decryption in C; UTF-8 paths on Windows.
- MAME-accurate 32-bit bus with byte lanes; DECO16IC ×2 with row/column scroll and
  partial-line updates; DECO 52 sprites ×2; DECO ACE palette, fades and alpha mixer.
- Sound: Z80 sound CPU, YM2151 (ymfm), two OKI M6295, 48 kHz stereo mix.
- Fallback interpreter for code the static analysis misses, recording entry points as
  seeds for the translator; LDM/STM user-bank and PSR semantics.
- Versioned, compressed, fully validated save states (CPU, board, EEPROM, sound, screen).
- SDL2 front end: main menu, in-game quick menu, ROM picker, 8 slots + autosave with
  previews and timestamps, quick save/load, display (scaling, filtering, VSync, rotation,
  scanlines, masks, brightness/gamma, borders, accents), audio (volume, mute, device,
  latency with rate control), controls (3 players, keyboard + controllers, remapping,
  presets, dead zone, hotplug, service/test), gadgets (FPS, frame time, input display),
  rewind, input recording/playback, screenshots, EEPROM persistence, logs.
- Windows x64 (MinGW-w64) and Linux AppImage packaging; headless test runner options
  (PNG/WAV capture, scripted input, states); scripted UI smoke test.

## [0.1.0] - 2026-10-31 — development snapshot, not playable

### Added

- Night Slashers parent (`nslasher`) and Japan (`nslasherj`) ROM verification with CRC32
  and SHA-1, exact MAME set identification and clone relationship.
- DECO156 program decryption, checked against MAME 0.289's debugger dump
  (65,536/65,536 words).
- DECO56/DECO74 tile decryption and tile ROM reordering.
- ARM2/DE156 instruction decoder, disassembler and recursive static analysis.
- Ahead-of-time ARM2-to-C translator with 26-bit PC/PSR, banked registers, exceptions,
  shifter carry, block transfers and DE156 coprocessor 0.
- Native CPU/bus runtime for DECO32: 128 KiB RAM, DECO104 protection and I/O table,
  DECO ACE palette/DMA, sprite buffers/DMA, DECO16IC backing stores, 93C46 model.
- Frame scheduler at 7 MHz / 57.828 Hz and a deterministic headless frame/hash harness.
- Pinned arcade-decomp submodule, build system, documentation and licence notices.

### Known limitations

- Boot stops in the EEPROM/settings verification path; no title screen or gameplay reached.
- Video renderer is a black-frame stub.
- Audio, SDL2 front end, controller mapping, EEPROM persistence and AppImage not implemented.
- No playable v1.0.0 release.
