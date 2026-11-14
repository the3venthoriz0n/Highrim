"""Checks on src/ that no unit test of game code can do (the game headers are not built into the tests).

Run: python -I tests/test_sources.py
"""
import glob
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")


def read(name):
    with open(os.path.join(SRC, name), encoding="utf-8") as handle:
        return handle.read()


def forms():
    return dict(re.findall(r"constexpr\s+std::uint32_t\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+);", read("Forms.h")))


class SourceTests(unittest.TestCase):
    def test_every_hold_note_id_matches_forms_h(self):
        # Rows of the hold table in Prison.cpp: { "Hold", faction, chest, jail, Forms::HighrimNote<Hold> },
        rows = re.findall(r'\{\s*"(\w+)"\s*,[^{}]*?Forms::(\w+)\s*\}', read("Prison.cpp"))
        self.assertEqual(len(rows), 9, "nine holds with a note")
        table = forms()
        for hold, note in rows:
            self.assertEqual(note, f"HighrimNote{hold}", f"{hold} points at the wrong note constant")
            self.assertIn(note, table, f"{note} is not in Forms.h")

    def test_lost_gear_table_matches_the_holds_and_forms_h(self):
        holds = [h for h, _ in re.findall(r'\{\s*"(\w+)"\s*,[^{}]*?Forms::(\w+)\s*\}', read("Prison.cpp"))]
        listed = re.findall(r"Forms::HighrimLostGear(\w+)", read("LostGear.cpp"))
        self.assertEqual(listed, holds, "LostGear.cpp lists one ability per hold, in the order of Prison::Holds()")
        table = forms()
        for hold in holds:
            self.assertIn(f"HighrimLostGear{hold}", table)

    def test_no_lookup_through_a_base_class_without_a_form_type(self):
        # TESDataHandler::LookupForm<T> checks form->Is(T::FORMTYPE). TESBoundObject (and MagicItem) have no form type
        # of their own, so the lookup always returned null: this is why no note was ever given.
        for path in glob.glob(os.path.join(SRC, "*.cpp")):
            text = open(path, encoding="utf-8").read()
            for base in ("TESBoundObject", "MagicItem", "TESForm", "TESObjectREFR"):
                self.assertNotRegex(text, rf"LookupForm(Raw)?<RE::{base}>", f"{os.path.basename(path)}: LookupForm<RE::{base}> never matches")


if __name__ == "__main__":
    unittest.main(verbosity=2)
