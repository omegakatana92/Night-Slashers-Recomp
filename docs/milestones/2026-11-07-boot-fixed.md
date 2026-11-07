# 2026-11-07 — boot blocker fixed

The "EEPROM/settings" blocker in `docs/KNOWN_ISSUES.md` was not an EEPROM timing
problem. Three translator/runtime bugs caused it:

1. **Shifter carry read before it was computed.** `movs r1, r1, lsl #1` was emitted as
   `nsl_logical(nsl_shift_imm(...), 1, nsl_cpu.shift_carry)`. C leaves argument
   evaluation order unspecified; GCC and MSVC read `shift_carry` before calling the
   shifter, so every flag-setting shifted operand used a stale carry. The EEPROM
   command loop (`0x3E7E4`) therefore sent `11 111111` instead of `10 aaaaaa`.
   The translator now evaluates the shifter into its own statement first.
2. **Byte addresses were word-aligned.** Load/store addresses and post-index
   writeback were masked with `0x03FFFFFC`, and `nsl_bus_read8/16`/`write8/16` did the
   same, so `ldrb r1, [r7, #1]` accessed `[r7]` and word loads never rotated.
   Byte/halfword accesses now use the 26-bit byte mask `0x03FFFFFF`; word stores are
   still aligned by the bus; writeback is not masked.
3. **Frames depended on the game taking the IRQ.** The scheduler ended a frame only on
   IRQ return and faulted after 8 frames with interrupts masked (the EEPROM write test
   needs ~110 ms with IRQs off). Frames are now fixed CPU time: 442 × 274 clocks at
   7 MHz (MAME `set_raw(28_MHz_XTAL / 4, 442, 0, 320, 274, 8, 248)`). IRQ0 is asserted
   at VBLANK start and stays asserted until the game writes `0x140000`
   (`vblank_ack_w`), matching `irq0_line_assert`.

The 93C46 model was also rewritten to follow `eepromser.cpp` exactly: no byte swap on
write, `1` shifted in after the 16 data bits, EWEN/EWDS lock, WRAL/ERAL, and READY/BUSY
timing from `eeprom.cpp` (1.75 ms write, 1 ms erase, 8 ms all).

## Check (Linux x86-64, gcc, nslasher Korea 1.3)

```
$ python3 tools/verify_rom.py nslasher.zip
OK: all 17 required files match MAME nslasher.
$ python3 tools/recompile.py nslasher.zip
analysed: 8046 instructions in 1506 blocks (3.1% of the 1 MiB program)
$ nsl_headless --frames 600 --save-eeprom ee.bin
frames=600 irqs=591 cycles=72653309 pc=0009C4 mode=0
vblank_acks=592 palette_dmas=3 sprite_dmas=2/2 unmapped=0/68
```

With a blank EEPROM the game initialises it; cell 0 becomes `0xC402`, the value seen in
MAME's `nvram/nslasher/eeprom`, the `CO20` signature is at cells `0x10–0x11`, and the
game reaches its user-mode main loop with VBLANK IRQs acknowledged every frame.
Video output is still a stub, so no picture has been verified yet.
