"""Verify a Night Slashers ROM archive without modifying it.

Usage:
    python tools/verify_rom.py rom/nslasher.zip
    python tools/verify_rom.py rom/nslasherj.zip --set nslasherj
    python tools/verify_rom.py rom/  --json build/rom.json

Prints every file's size, CRC32 and SHA-1, then whether the archive is the exact MAME
parent set (nslasher, Korea Ver. 1.3) or the Japanese clone (nslasherj, Japan Ver. 1.2).
The PLDs vm-00.3d/vm-01.4d are NO_DUMP in MAME and are not required; present copies are
reported but do not fail verification.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nsl_rom


def show(path: Path, set_name: str | None) -> nsl_rom.RomSet:
    rom = nsl_rom.inspect(path, set_name)
    print(f"archive : {path}")
    print(f"set     : {rom.name} - {rom.title}")
    print(f"mame    : {rom.mame}" + (f" (clone of {rom.parent})" if rom.parent else " (parent)"))
    print()
    print(f"{'file':16s} {'size':>9s}  {'CRC32':8s}  SHA-1")
    print("-" * 78)
    for name, check in rom.checks.items():
        if not check.present:
            print(f"{name:16s} {'MISSING':>9s}  {check.expected.crc:08x}  {check.expected.sha1}")
            continue
        marks = []
        if not check.size_ok:
            marks.append(f"size {check.size} != {check.expected.size}")
        if not check.crc_ok:
            marks.append(f"crc {check.crc:08x} != {check.expected.crc:08x}")
        if not check.sha1_ok:
            marks.append("sha1 differs")
        suffix = ("  <-- " + ", ".join(marks)) if marks else ""
        print(f"{name:16s} {check.size:9d}  {check.crc:08x}  {check.sha1}{suffix}")
    if rom.extra:
        print()
        print("extra files (ignored): " + ", ".join(rom.extra))
    print()
    if rom.complete:
        print(f"OK: all {len(rom.checks)} required files match MAME {rom.mame}.")
    else:
        if rom.missing():
            print("MISSING: " + ", ".join(rom.missing()))
        if rom.incorrect():
            print("INCORRECT: " + ", ".join(rom.incorrect()))
        print("FAILED: this is not the exact MAME set for this region.")
    return rom


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="nslasher.zip, nslasherj.zip, or a directory")
    parser.add_argument("--set", choices=("nslasher", "nslasherj"), help="force the expected region")
    parser.add_argument("--json", type=Path, help="also write the inventory as JSON")
    args = parser.parse_args(argv)
    try:
        rom = show(args.rom, args.set)
    except nsl_rom.RomError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    if args.json:
        payload = {
            "archive": str(args.rom),
            "set": rom.name,
            "title": rom.title,
            "mame": rom.mame,
            "parent": rom.parent,
            "complete": rom.complete,
            "files": {
                name: {
                    "expected_size": check.expected.size,
                    "size": check.size,
                    "expected_crc": f"{check.expected.crc:08x}",
                    "crc": f"{check.crc:08x}",
                    "expected_sha1": check.expected.sha1,
                    "sha1": check.sha1,
                    "present": check.present,
                    "ok": check.ok,
                }
                for name, check in rom.checks.items()
            },
        }
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return 0 if rom.complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
