# DECO32 / Night Slashers hardware

Sources: MAME 0.289 `src/mame/dataeast/fghthist.cpp`, `deco156.cpp`, `deco104.cpp`,
`deco146.cpp`, `decocrpt.cpp`, `deco_ace.cpp`, `deco16ic.cpp`, `decbac06.cpp`,
`decmxc06.cpp`, `src/devices/cpu/arm7/` and `src/devices/machine/eepromser.cpp` (all
BSD-3-Clause). MAME is a development reference only and is not linked into the port.

## Board

- Data East DE-0397-0 PCB, 1994.
- Main CPU: **Data East 156 (DE156)** — an ARM2 core in 26-bit mode (MAME
  `de156_cpu_device` derives from `arm2_cpu_device`, `ARCHFLAG_MODE26 | ARCHFLAG_ONLY26`),
  clocked at 28 MHz / 4 = **7 MHz**.
  - No Thumb, no halfword transfers, no `BX`, no long multiply.
  - Coprocessor 0 adds BCD add/multiply/subtract and 32-bit divide/remainder.
  - The program ROM is encrypted; MAME decrypts the whole 1 MiB region in
    `deco156_decrypt()` before reset.
- Work RAM: 128 KiB at `0x100000-0x11FFFF`.
- Video: 320×240 visible, 442×274 total at 7 MHz pixel clock, 57.828 Hz. Three tile layers
  through two `deco16ic` devices, two `decbac06`/`decmxc06` sprite generators, DECO ACE
  palette/DMA.
- Sound: Z80 at 32.22 MHz / 9 = 3.58 MHz, YM2151 at the same clock, two OKI M6295 at
  32.22/32 and 32.22/16 MHz. Sound program `sndprg.17l`; samples `mbh-10.14l` and
  `mbh-11.16l`.
- Inputs: active-low IN0/IN1 through the DECO104 I/O/protection chip, 93C46 serial EEPROM.
- Interrupt: VBLANK autovector, ARM IRQ vector `0x18`.
- Frame: 278,016 CPU clocks at 16 MHz for Cave boards is **not** this board. Here a frame
  is 7,000,000 / 57.828 ≈ **121,050 DE156 clocks**.

## Memory map (byte addresses)

| Address | Device |
| --- | --- |
| `0x000000-0x0FFFFF` | decrypted ARM program ROM |
| `0x100000-0x11FFFF` | work RAM |
| `0x120000-0x1200FF` | ACIA (unused, nop) |
| `0x140000` | VBLANK acknowledge (write) |
| `0x150000` | 93C46 EEPROM control (write), DO on read |
| `0x150001` | volume (write) |
| `0x163000-0x16309F` | DECO ACE registers/RAM |
| `0x164000/4/8` | tile / sprite1 / sprite2 colour bank |
| `0x168000-0x169FFF` | buffered palette RAM |
| `0x16C008` | palette DMA trigger |
| `0x170000-0x171FFF` | sprite generator 0 RAM |
| `0x174010` | sprite 0 buffer DMA |
| `0x178000-0x179FFF` | sprite generator 1 RAM |
| `0x17C010` | sprite 1 buffer DMA |
| `0x182000-0x185FFF` | DECO16IC #0 VRAM (8×8 and 16×16 pages) |
| `0x192000-0x195FFF` | DECO16IC #0 row scroll |
| `0x1A0000-0x1A001F` | DECO16IC #0 control |
| `0x1C2000-0x1C5FFF` | DECO16IC #1 VRAM |
| `0x1D2000-0x1D5FFF` | DECO16IC #1 row scroll |
| `0x1E0000-0x1E001F` | DECO16IC #1 control |
| `0x200000-0x207FFF` | DECO104 protection / I/O (16-bit lanes) |

## ROM sets

`nslasher` is the parent (MAME calls it **Korea Ver. 1.3**); `nslasherj` is the Japan
Ver. 1.2 clone. Both use the same DE-0397-0 board. Only the ARM program pair differs:

```text
nslasher : mainprg.1f (507acbae) mainprg.2f (931fc7ee)
nslasherj: lx-00.1f   (6ed5fb88) lx-01.2f   (a6df2152)
```

The program region is built from the pair exactly as MAME's `ROM_LOAD32_WORD`: bytes 0-1
of every 32-bit little-endian word from file 1, bytes 2-3 from file 2. The file offset
advances by 2 bytes per word, producing 1 MiB from two 512 KiB files.

## DECO156 decryption

MAME's `deco156_decrypt()` is ported in `tools/nsl_rom.py`. It was checked against a
memory dump produced by MAME 0.289's debugger: all 65,536 words of the parent set match.
The first words decrypt to the ARM vector table:

```text
EA000012 EA000005 EA000006 EA000007 EA000008 EA000009 EA0000BA EA000009
```

## Graphics decryption

MAME's `deco56_decrypt_gfx()` and `deco74_decrypt_gfx()` operate on 16-bit big-endian words
using three 0x800-entry tables each (`xor_table`, `address_table`, `swap_table`) plus
`xor_masks` and eight 16-bit swap patterns. The tables are reproduced in
`tools/deco_gfx_tables.py` with attribution. `nslasher_state::reorder_tilegfx()` swaps the
`0x080000-0x100000` and `0x100000-0x180000` halves of both tile ROMs before decryption.

Sprite ROMs are not encrypted in the same way; the generator layouts are 5bpp for
`sprites1` and 4bpp for `sprites2`. The renderer is not implemented yet.

## DECO104 protection / I/O

The 2048-entry (indexed as 1024 cells) table from `deco104.cpp` is reproduced in
`runtime/src/deco104_table.h`. It maps internal 0x80-word RAM and input ports A/B/C
through nibble permutations, XOR and NAND masks. The driver uses the "interleave"
address scramble `(4,5,3,6,2,7,1,8,0,9)`. Input port A is IN0, B is EEPROM DO, C is IN1.
Ports and the internal RAM are accessed through `0x200000-0x207FFF`; the game polls IN1
bit 4 for VBLANK.

## Known timing facts used by the runtime

- IRQ vector `0x18` branches to `0x308`; the handler saves r0-r12/lr with `STMIA`, uses
  IRQ SP `0x11FE00`, acknowledges VBLANK at `0x140000`, and returns with
  `SUBS PC, LR, #4`.
- VBLANK is active for the 34 non-visible lines: 34 × 442 = 15,028 clocks.
- The 26-bit PC/PSR rules follow MAME's `write_r15_psr26()`: PC reads used as a data
  processing `Rn` are masked to 26 bits (so `BICS PC,PC,#3` clears I/F/mode), while an
  exception return value carries the interrupted PSR in bits 31:26 and 1:0.
