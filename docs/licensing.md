# Licensing review

## Repository licence

The GitHub repository was created with a **GNU General Public License v2.0** `LICENSE`.
That file is preserved unchanged, as requested, until the licensing review is complete.
Original source files in this repository carry `SPDX-License-Identifier: GPL-2.0-only`.

This project was proposed with PolyForm licensing for original code. PolyForm is **not**
applied now: changing the repository's licence is a legal decision for the repository
owner, and the existing GPL-2.0 text must not be replaced without that review. If the owner
later chooses PolyForm for original code, the third-party BSD-3-Clause material listed
below must remain available under BSD-3-Clause and the result must not misrepresent the
upstream terms.

## Third-party components

### arcade-decomp (`tools/core`)

- Upstream: https://github.com/cheviman/arcade-decomp
- Branch/commit pinned: `tmnt` at `ed4e157b4bb3d34b6555766262c70d6f010ba619`
- Licence: BSD-3-Clause, copyright Sebastian Toyos (see `tools/core/LICENSE`).
- It is a git submodule and is not modified by this repository.

### MAME-derived material

The following are derived from MAME 0.289 sources, all marked BSD-3-Clause in MAME:

- DECO156, DECO56, DECO74 decryption algorithms and tables;
- DECO104 protection lookup table;
- memory map, clock and timing facts;
- ARM2/DE156 26-bit exception and PSR semantics;
- 93C46 serial EEPROM protocol notes.

They are used as development references and as hardware-behaviour descriptions; no MAME
code is linked into the shipped game target. MAME as a whole is GPL-2.0-or-later, but the
individual files used are BSD-3-Clause. See `LICENSES/BSD-3-Clause-MAME.txt`.

### ROM data

No ROM data, extracted graphics/audio, generated translated C, screenshots containing game
art, EEPROM images or NVRAM dumps are committed. The player supplies the verified ROM set.
Generated files live under `build/` and are ignored by Git.

## Attribution in releases

Any future release archive must contain this `LICENSES/` directory, the GPL-2.0 `LICENSE`,
the arcade-decomp BSD-3-Clause notice and the MAME BSD-3-Clause notice.
