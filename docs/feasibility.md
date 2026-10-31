# Feasibility and implementation plan

Date: 31 October 2026. This is the Phase 1 report, updated with what was actually built.

## 1. Upstream investigation

`cheviman/arcade-decomp` was inspected at both `origin/main` (newest) and the `tmnt` branch
used by the existing DoDonPachi port. It provides:

- CPU frontends for NEC V60/V70, Motorola 68000 and Z80;
- a decompiler IR and C emitter (`v60/ir.py`, `v60/emit_c.py`, `decompiler/`);
- static analysis and translation scaffolds (`m68000/translate.py`, `recompile.py`);
- reference harness patterns that run MAME CPU/video/sound code for verification
  (`konami/reference/`, `system32/reference/`);
- ROM verification and scenario tooling.

It has **no ARM frontend** and **no Data East DECO32 board runtime**. The V60 and 68000
frontends cannot execute ARM code; their decoders, register models and IR do not describe
ARM condition codes, the 26-bit PC/PSR, or the DE156 coprocessor.

The `tmnt` branch was pinned because it contains the 68000/Z80 translation scaffold and
scenario conventions this project's tooling follows. `origin/main` has since removed the
68000 translator while reworking the V70/decompiler path, so it is not a drop-in base for
this port.

## 2. Hardware findings

Night Slashers runs on Data East's DE-0397-0 DECO32 board:

- DE156 main CPU: **ARM2 in 26-bit mode at 7 MHz**, encrypted 1 MiB program.
- Z80 sound CPU at 3.58 MHz, YM2151 + two OKI M6295, 64 KiB sound program, 1.5 MiB samples.
- Two DECO16IC tile generators, two DECO sprite generators, DECO ACE palette DMA,
  320×240 at 57.828 Hz.
- DECO104 protection/I/O at `0x200000`, 93C46 EEPROM.
- Two supported sets: `nslasher` (parent, Korea 1.3) and `nslasherj` (Japan 1.2), differing
  only in the program ROM pair.

Details and sources are in [`hardware.md`](hardware.md).

## 3. Capabilities missing from arcade-decomp and what was added

| Missing capability | Implementation |
| --- | --- |
| ARM2/DE156 instruction decoder and disassembler | `tools/arm2/decode.py`, `disassembler.py` |
| ARM static analysis / control-flow discovery | `tools/arm2/analysis.py` |
| ARM → C translation with 26-bit/PSR semantics | `tools/arm2/translate.py` |
| DE156 coprocessor 0 (BCD, divide) | `runtime/src/cpu.c` |
| DECO156 program decryption | `tools/nsl_rom.py` |
| DECO56/DECO74 graphics decryption | `tools/nsl_rom.py`, `tools/deco_gfx_tables.py` |
| DECO104 protection/I/O table | `runtime/src/machine.c`, `runtime/src/deco104_table.h` |
| DECO32 board model | `runtime/src/machine.c` |
| Frame/VBLANK model at 7 MHz | `runtime/src/run.c`, `runtime/include/nsl/cpu.h` |
| Boot/frame harness | `ports/headless/main.c` |

The ARM frontend is intentionally game-local (`tools/arm2/`) until it is upstreamable; the
shared core stays pristine except for the pinned submodule.

## 4. Translation strategy

The translator is ahead-of-time and produces one C function per basic block:

- roots are the eight exception vectors; direct `B`/`BL` targets and conditional
  fall-throughs extend the graph;
- PC-relative loads identify literal pools (data, never decoded as code);
- a load into PC from a literal resolves a computed jump target;
- block-local constant tracking resolves `LDR`/`MOV`/`ADR`-style function-pointer calls;
- every emitted instruction becomes C using helpers for exact flags, shifts, loads,
  stores, block transfers, exceptions and coprocessor operations;
- no opcode is decoded at run time; an untranslated PC is a reported fault.

ARM2 quirks implemented and checked against MAME's source:

- 26-bit PC; `Rn == PC` in data processing uses the masked PC, while exception returns
  carry PSR bits in 31:26 and 1:0;
- `STM`/`LDM` addressing modes IB/IA/DB/DA and base-in-list writeback suppression;
- banked r13/r14 and FIQ r8-r12, 26-bit exception link format;
- unaligned word loads rotate;
- shifter carry, RRX, register-specified shifts;
- DE156 BCD add/multiply/subtract and divide/remainder.

## 5. Verification plan

1. **ROM and decryption** — CRC32/SHA-1 against MAME; decrypt and compare with MAME's
   debugger memory dump. Done: 65,536/65,536 words.
2. **Instruction semantics** — differential tests against MAME's ARM core for the ARM2
   subset. Not yet automated beyond the decryption and boot observations.
3. **Boot** — run translated code with the board model and compare PC/RAM against a MAME
   trace. In progress; blocked in the EEPROM/settings path.
4. **Video** — port DECO16IC and sprite generators; compare PNG frame hashes with MAME.
5. **Sound** — reuse arcade-decomp's Z80 frontend; add YM2151 and MKS6295; compare WAV.
6. **Front ends** — SDL2, ROM selection, controller mapping, EEPROM persistence.
7. **Release** — Windows zip, Linux AppImage, CI, checksums. Only after playable tests.

## 6. Risks and blockers

- The EEPROM bit-banging timing is the current boot blocker. Details and evidence are in
  [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md).
- The sprite/tile renderers are the largest remaining implementation item.
- No automated MAME differential harness exists yet; the reference traces used so far were
  produced manually with MAME's debugger.
- The ARM frontend has not yet been validated instruction-by-instruction against MAME's
  ARM core, only through boot behaviour and decryption.
- A playable v1.0.0 release is therefore **not** justified.
