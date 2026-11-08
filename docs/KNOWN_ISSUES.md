# Known issues and limitations (0.2.0)

## Not tested on real hardware
- No test on a physical Windows 11 PC, Steam Deck, or with physical Xbox/PlayStation
  controllers or arcade sticks. Builds were verified with a virtual display (Xvfb), Wine for
  the Windows executables, scripted input and deterministic hashes. Controller hotplug,
  remapping and Steam Deck controls use standard SDL2 GameController code paths but are
  untested with devices.
- The MSVC build is not verified (the Windows package is built with MinGW-w64).

## Emulation accuracy
- **CPU timing** is one cycle per ARM instruction. The game is VBLANK-locked, so speed is
  correct, but boot and some sequences land 1-2 frames away from MAME.
- **Sound CPU**: the Z80 program runs on the superzazu/z80 interpreter; unlike the ARM program
  it is not yet translated ahead of time. Sound latch writes are replayed at their exact time
  each frame (the Z80 cannot talk back to the ARM on this board).
- Audio was compared with MAME by level, envelope and spectrum (envelope correlation 0.92,
  same spectral balance); it has not been compared by ear against the arcade board.
- Video follows MAME's `mix_nslasher`, including its own TODOs (object alpha priority 2/3).
- DE156 coprocessor instructions are implemented as before but have not been seen in use.

## Code coverage
- Static analysis plus recorded seeds (`tools/traced_targets.txt`, 1,134 entry points) cover
  attract mode and several scripted play sessions. Code that is still unseen runs through the
  fallback interpreter (`runtime/src/interp.c`) with identical semantics; addresses it meets
  are appended to `missed_blocks.txt` in the data folder so they can be added as seeds.

## Front end
- **Not supported:** CRT curvature and widescreen. The renderer is SDL2's 2D API (no shader
  pipeline); scanlines and aperture/slot masks are done with multiply-blended overlays. The
  board draws a fixed 320x240 picture, so widescreen would require inventing image content.
- **Score / lives / stage gadgets are not provided.** Their RAM locations have not been
  verified; only FPS, frame time and input display are shown, as required.
- Separate music/effects volume is not possible: the board mixes them in hardware.
- Only the parent set `nslasher` (Korea Ver. 1.3) runs. The Japan set (`nslasherj`) is
  recognised and refused with an explanation, because each program revision needs its own
  translation; no version selector is shown.
- The menu font is an upscaled 8x8 bitmap font; non-Latin file names show `?` per character.

## Distribution
- Release executables contain C translated from the program ROM (see `docs/licensing.md`).

## Fixed in 0.2.0
- The boot blocker (EEPROM check) — see `docs/milestones/2026-11-07-boot-fixed.md`.
