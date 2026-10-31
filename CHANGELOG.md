# Changelog

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
