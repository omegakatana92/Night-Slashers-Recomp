"""Translate the player's Night Slashers ROM into native C.

Usage:
    python tools/recompile.py rom/nslasher.zip
    python tools/recompile.py rom/nslasherj.zip --set nslasherj

Writes build/generated/program*.c, build/generated/program.h and build/assets/program.bin.
The generated C is the game's translated program and must never be committed or shipped;
it is built locally from the player's own ROM.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import nsl_rom
from arm2.analysis import Analysis
from arm2.translate import Program, write_program


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="nslasher.zip or nslasherj.zip")
    parser.add_argument("--set", choices=("nslasher", "nslasherj"), help="expected region")
    parser.add_argument("--out", type=Path, default=TOOLS.parent / "build" / "generated")
    parser.add_argument("--assets", type=Path, default=TOOLS.parent / "build" / "assets")
    args = parser.parse_args(argv)

    try:
        rom = nsl_rom.load(args.rom, args.set)
    except nsl_rom.RomError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    image = nsl_rom.build_program(rom)
    analysis = Analysis(image).run()
    summary = analysis.summary()
    print(f"ROM: {rom.title}")
    print(f"analysed: {summary['instructions']} instructions in {summary['blocks']} blocks "
          f"({summary['coverage']:.1%} of the 1 MiB program)")
    print(f"functions: {summary['functions']}  literal words: {summary['literals']}  "
          f"indirect PC sites: {summary['indirect_pc_sites']}")

    program = Program(image=image, analysis=analysis, region=rom.title)
    stats = write_program(program, args.out)

    args.assets.mkdir(parents=True, exist_ok=True)
    (args.assets / "program.bin").write_bytes(image)
    (args.assets / "program.region").write_text(rom.name, encoding="ascii")
    report = {
        "rom": rom.name,
        "title": rom.title,
        "program_sha1": __import__("hashlib").sha1(image).hexdigest(),
        **stats,
    }
    (TOOLS.parent / "build" / "recompile_report.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8")
    print(f"wrote {stats['blocks']} blocks ({stats['instructions']} instructions) to {args.out}")
    print(f"decrypted program image: {args.assets / 'program.bin'}")
    if summary["indirect_pc_sites"]:
        print(f"note: {summary['indirect_pc_sites']} PC writes are resolved only at run time; "
              f"the dispatcher reports any address without a translated block.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
