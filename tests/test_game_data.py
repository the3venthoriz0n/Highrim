"""Checks that src/GameData.h is what tools/extract_game_data.py generates from Skyrim.esm, and that the
values are sane. Skips when Skyrim.esm is not available (the committed header is then not re-verified).

Run: python -I tests/test_game_data.py
"""
import os
import re
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "extract_game_data.py")
HEADER = os.path.join(ROOT, "src", "GameData.h")
SKYRIM_ESM = os.environ.get(
    "HIGHRIM_SKYRIM_ESM",
    r"C:\Users\Andrew\Documents\My Games\_STEAM\steamapps\common\Skyrim Special Edition\Data\Skyrim.esm",
)


class HeaderShape(unittest.TestCase):
    """Runs without the game."""

    def setUp(self):
        with open(HEADER, encoding="utf-8") as handle:
            self.text = handle.read()

    def test_holds_have_anchors(self):
        rows = re.findall(r"\{ 0x([0-9A-F]{8}), (-?\d+)\.0f, (-?\d+)\.0f, 0x([0-9A-F]{8}) \},  // (\w+):", self.text)
        self.assertEqual([r[4] for r in rows], ["Whiterun", "Eastmarch", "Rift", "Haafingar", "Hjaalmarch", "Reach",
                                                 "Falkreath", "Pale", "Winterhold"])
        for _faction, x, y, _city, name in rows:
            self.assertLess(abs(int(x)), 250000, name)
            self.assertLess(abs(int(y)), 250000, name)
        cities = {r[4]: r[3] for r in rows}
        for name in ("Whiterun", "Eastmarch", "Rift", "Haafingar", "Reach"):
            self.assertNotEqual(cities[name], "00000000", name)
        for name in ("Hjaalmarch", "Falkreath", "Pale", "Winterhold"):
            self.assertEqual(cities[name], "00000000", name)

    def test_markers_sorted_and_unique(self):
        block = self.text.split("kTamrielMarkers[] = {")[1]
        ids = [int(i, 16) for i in re.findall(r"\{ 0x([0-9A-F]{8}), -?\d+\.0f, -?\d+\.0f \}", block)]
        self.assertGreater(len(ids), 200)
        self.assertEqual(ids, sorted(set(ids)))


@unittest.skipUnless(os.path.exists(SKYRIM_ESM), "Skyrim.esm not found")
class AgainstSkyrimEsm(unittest.TestCase):
    def test_header_matches_the_master_file(self):
        result = subprocess.run([sys.executable, "-I", TOOL, "--check", SKYRIM_ESM], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
