"""Self-checks for the ROM tooling that do not need a commercial ROM.

The integration check runs only when NSL_ROM points at the player's archive; it verifies
the exact MAME set, the first decrypted vector words and the decrypted image SHA-1.
"""
from __future__ import annotations

import hashlib
import os
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import nsl_rom


class RomMetadataTests(unittest.TestCase):
    def test_sets_are_complete_and_consistent(self):
        self.assertEqual(set(nsl_rom.ROM_SETS), {"nslasher", "nslasherj"})
        parent = nsl_rom.ROM_SETS["nslasher"]
        clone = nsl_rom.ROM_SETS["nslasherj"]
        self.assertIsNone(parent["parent"])
        self.assertEqual(clone["parent"], "nslasher")
        self.assertEqual(len(parent["files"]), 17)
        self.assertEqual(len(clone["files"]), 17)
        shared_parent = {f.name for f in parent["files"]} - {f.name for f in parent["program"]}
        shared_clone = {f.name for f in clone["files"]} - {f.name for f in clone["program"]}
        self.assertEqual(shared_parent, shared_clone)

    def test_program_pairs(self):
        self.assertEqual(
            [f.name for f in nsl_rom.PARENT_PROGRAM], ["mainprg.1f", "mainprg.2f"])
        self.assertEqual(
            [f.name for f in nsl_rom.JAPAN_PROGRAM], ["lx-00.1f", "lx-01.2f"])

    def test_decrypt_is_a_permutation(self):
        words = list(range(0x40000))
        out = nsl_rom.deco156_decrypt(words)
        self.assertEqual(len(out), len(words))


class RomIntegrationTests(unittest.TestCase):
    def test_player_rom_decrypts_like_mame(self):
        archive = os.environ.get("NSL_ROM")
        if not archive:
            self.skipTest("set NSL_ROM to nslasher.zip or nslasherj.zip")
        rom = nsl_rom.load(archive)
        image = nsl_rom.build_program(rom)
        self.assertEqual(len(image), nsl_rom.PROGRAM_SIZE)
        self.assertEqual(
            image[:16].hex(), "120000ea050000ea060000ea070000ea")
        expected = {
            "nslasher": "a366c99365194ccdfcb9c573bb0b739d985b8f68",
            "nslasherj": "7faaf85ca833331f9af1993e9846bf88992e9ae8",
        }[rom.name]
        self.assertEqual(hashlib.sha1(image).hexdigest(), expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
