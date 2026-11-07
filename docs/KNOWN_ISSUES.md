# Known issues

## Fixed 2026-11-07: boot no longer stops in the EEPROM/settings path

See `docs/milestones/2026-11-07-boot-fixed.md`. The game now initialises a blank
EEPROM, passes its checks and runs its main loop with VBLANK IRQs. The original
analysis is kept below for history.

## Current limitations

- Video renderer is not yet a DECO32 renderer (work in progress).
- Audio, SDL2 front end, controller mapping and AppImage are not implemented yet.

---

## (historical) Boot stopped in the EEPROM/settings path

## Boot stops in the EEPROM/settings path

Observed with the parent set (`nslasher`, Korea 1.3):

```
python tools/recompile.py rom/nslasher.zip
build\vs\bin\nsl_headless.exe --frames 1
Night Slashers CPU fault at 03EB74: frame did not reach vblank within 968400 cycles
```

Without the development EEPROM image the PC is `0x3E978`; with MAME's valid EEPROM image
loaded, the boot still reaches the EEPROM test/error block at `0x3EB44-0x3EB74`.

What this block does (26-bit ARM2 code at `0x3E7A4`):

1. Bit-bangs the 93C46 on `0x150000`: CS = bit 6, CLK = bit 5, DI = bit 4; DO is read
   through DECO104 port B (`0x2006B4`, input port C/IN1 or B depending on the table entry).
2. Writes a walking pattern (`0x2323`, `0x0C0C`, `0xC6C6`, …) to all 64 cells and reads it
   back into RAM `0x11FD00`.
3. Copies `0x11FD00..0x11FD7F` to `0x11FD80..0x11FDFF` and compares byte for byte at
   `0x3EB20`; mismatch enters the error path at `0x3EB44`.
4. A valid EEPROM takes the checksum path at `0x3E8B4`/`0x3E8CC` and returns at `0x3E994`.
   MAME's trace follows that path; the native runtime currently follows the error path.

Evidence collected:

- MAME 0.289 debugger trace (200 ms and 500 ms) executes
  `0x3E7A4 → 0x3E8B4 → 0x3E8CC → 0x3E994` and does **not** execute `0x3EB20` or `0x3E978`.
- MAME's EEPROM image after a normal boot is `nvram/nslasher/eeprom` (128 bytes).
- The native runtime's `0x11FD00` buffer after a read of that image contains bytes such as
  `C4 00 00 00 ...` where the expected first cell (`0xC402`) should appear as `02 C4`.
  The read routine shifts the DO bitstream; the exact number of leading dummy bits and the
  sampling edge are not yet matched.
- Changing the output edge, bit order, dummy-bit count and write byte/bit swap moves the
  mismatch but has not produced the MAME buffer. These experiments are not committed as
  final behaviour.

The 93C46 model in `runtime/src/machine.c` follows MAME's `eepromser.cpp` command state
machine (start bit, 2-bit command, 6-bit address, 16-bit data, byte swap on write), but the
DO timing relative to this board's bit-banging is not yet correct.

## Consequence

- No playable frame, title screen or gameplay has been observed.
- The video renderer is a black-frame stub, not a DECO32 renderer.
- Audio, SDL2, controller input, EEPROM persistence and AppImage packaging are not
  implemented.
- No release tagged `v1.0.0` will be created until these are fixed and tested.

## Next steps for the blocker

1. Build a small standalone 93C46 test that feeds the exact `0x150000` write sequence from
   the MAME trace and compares the sampled DO bitstream with MAME's.
2. Alternatively, compile MAME's `eepromser.cpp` with a stub into a reference helper and
   dump its DO transitions for the same sequence.
3. Once the pattern test passes, verify the checksum path and the first graphics
   initialization (palette DMA and sprite DMA counts should rise).
4. Then implement DECO16IC tile rendering and the two sprite generators, comparing PNG
   hashes with MAME.

## Other limitations

- Static analysis currently covers 8,046 instructions / 1,506 blocks (about 3% of the 1 MiB
  program image). More code will be discovered as indirect function-pointer tables are
  analysed; the dispatcher reports any missing PC instead of guessing.
- The ARM translator has not been differential-tested instruction by instruction against
  MAME's ARM core.
- The Windows build uses Visual Studio 18 / MSVC; MinGW/Ninja also configures. The Linux
  build and AppImage have not been produced.
