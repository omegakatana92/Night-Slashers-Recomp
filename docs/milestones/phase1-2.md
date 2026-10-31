# Phase 1-2 milestone log (31 October 2026)

## Commands executed

```powershell
python tools\verify_rom.py C:\Users\omega\Downloads\nslasher.zip
python tools\verify_rom.py C:\Users\omega\Downloads\nslasherj.zip
python tools\recompile.py C:\Users\omega\Downloads\nslasher.zip
build\vs\bin\nsl_headless.exe --frames 60
```

## Results

### ROM verification

Parent `nslasher`:

```
OK: all 17 required files match MAME nslasher.
```

Files verified (CRC32 / SHA-1): `mainprg.1f`, `mainprg.2f`, `sndprg.17l`, `mbh-00.8c`,
`mbh-01.9c`, `mbh-02.14c`, `mbh-04.16c`, `mbh-06.18c`, `mbh-03.15c`, `mbh-05.17c`,
`mbh-07.19c`, `mbh-08.16e`, `mbh-09.18e`, `mbh-10.14l`, `mbh-11.16l`, `ln-00.j7`,
`vm-02.8j`.

Japan `nslasherj`:

```
OK: all 17 required files match MAME nslasherj.
```

Only the program pair differs (`lx-00.1f`, `lx-01.2f`); all shared files have identical
CRCs and SHA-1s. `nslasherj` is a clone of `nslasher` in MAME. `vm-00.3d` and `vm-01.4d`
are `NO_DUMP` and absent; they are not required.

### DECO156 decryption

The decryptor was checked against a 1 MiB memory dump from MAME 0.289's debugger:

```
matches 65536 of 65536 words
ours@0      0xEA000012
ours@0x92C6 0xE5C80032
```

Decrypted image hashes:

```
nslasher   1,048,576 bytes  SHA-1 a366c99365194ccdfcb9c573bb0b739d985b8f68
nslasherj  1,048,576 bytes  SHA-1 7faaf85ca833331f9af1993e9846bf88992e9ae8
```

First vector words (little-endian bytes): `12 00 00 ea 05 00 00 ea 06 00 00 ea 07 00 00 ea`.

### ARM frontend and translation

```
analysed: 8046 instructions in 1506 blocks
functions: 59  literal words: 522  indirect PC sites: 133
wrote 1506 blocks (9647 instructions) to build\generated
```

The ARM2 decoder handles ARM2 data processing, multiply, single/block transfers, branches,
coprocessor 0 and SWI; there is no Thumb decoder because the DE156 has no Thumb.

### Headless boot

```
frames=60 irqs=60 cycles=7278046 pc=0056B8
vblank_acks=62 palette_dmas=3 sprite_dmas=3/3
```

No invalid bus access and no missing translated block were reported in this run; the boot
nevertheless stops in the EEPROM/settings path (see `docs/KNOWN_ISSUES.md`). This is
**not** a successful boot milestone: no title screen or gameplay was reached.

## Conclusion

Phase 1 (investigation/feasibility) and Phase 2 (ROM verification) are complete. Phase 3
(ARM frontend + board runtime) is substantially implemented but blocked on the 93C46
bit-timing, so Phases 4-8 (playable front ends, AppImage, releases) are not complete and no
playable release is published.
