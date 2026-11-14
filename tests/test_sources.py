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

    def test_clue_texts_use_the_capital_and_never_say_prison(self):
        # The note, the wake-up message and Lost Gear name the capital city only (see Core::CapitalName).
        core = read("Core.cpp")
        for fn in ("LostGearMessage", "NoteText"):
            body = core[core.index(f"std::string {fn}("):]
            body = body[:body.index("    }")]
            self.assertNotIn("prison", body, f"{fn} must not say prison")
        blackout = read("Blackout.cpp")
        self.assertIn("Core::NoteText(Core::CapitalName(", blackout)
        self.assertIn("Core::LostGearMessage(Core::CapitalName(", blackout)

    def test_risk_ability_ids_are_in_forms_h(self):
        table = forms()
        doses = read("Doses.cpp")
        for tier in ("Low", "Elevated", "High"):
            self.assertIn(f"Forms::HighrimOverdose{tier}", doses)
            self.assertIn(f"HighrimOverdose{tier}", table)
            self.assertIn(f"HighrimAbOverdose{tier}", table)
        self.assertIn("HighrimImodOverdose", table)
        self.assertIn("Forms::HighrimImodOverdose", doses)

    def test_the_faint_moves_nothing(self):
        # The faint is the mild outcome: no items to a chest, no teleport, no note, no Lost Gear reminder, no 6 h skip.
        text = read("Blackout.cpp")
        body = text[text.index("void DoFaintWork()"):]
        body = body[:body.index("        void Tick()")]
        for forbidden in ("RemoveItem", "MoveTo", "AddObjectToContainer", "LostGear::", "Prison::", "Rollback", "6.0f"):
            self.assertNotIn(forbidden, body, f"the faint must not use {forbidden}")
        self.assertIn("Core::kFaintGameHours", body)
        self.assertIn("Core::FaintHangoverExpiry", body)

    def test_a_rolled_full_blackout_becomes_a_faint_when_the_full_blackout_is_off(self):
        doses = read("Doses.cpp")
        self.assertIn("Overdose roll: full -> faint (full blackout is off)", doses)
        self.assertIn("Blackout::RunFaint", doses)
        # the only way to the full blackout from a dose goes through the allowed check
        self.assertIn("FullBlackoutAllowed()", doses)
        self.assertIn("if (!FullBlackoutAllowed())", read("Blackout.cpp"))

    def test_no_lookup_through_a_base_class_without_a_form_type(self):
        # TESDataHandler::LookupForm<T> checks form->Is(T::FORMTYPE). TESBoundObject (and MagicItem) have no form type
        # of their own, so the lookup always returned null: this is why no note was ever given.
        for path in glob.glob(os.path.join(SRC, "*.cpp")):
            text = open(path, encoding="utf-8").read()
            for base in ("TESBoundObject", "MagicItem", "TESForm", "TESObjectREFR"):
                self.assertNotRegex(text, rf"LookupForm(Raw)?<RE::{base}>", f"{os.path.basename(path)}: LookupForm<RE::{base}> never matches")


    def test_task18_timed_and_ravage_ids_are_in_forms_h(self):
        header = read("Forms.h")
        withdrawal = read("Withdrawal.cpp")
        for name in ("kTimedAddicted", "kTimedWithdrawal", "kTimedVariantCount"):
            self.assertIn(name, header)
            self.assertIn(f"Forms::{name}", withdrawal)
        self.assertIn("HighrimRavage", forms())
        self.assertIn("Forms::HighrimRavage", withdrawal)
        self.assertIn("kDrainUsesRavage", read("Core.h"))

    def test_rimerush_has_no_high_screen_effect(self):
        self.assertNotIn("kImodStimulant", read("Doses.cpp") + read("Forms.h"))
        self.assertIn("Core::HasHighVisual(type)", read("Doses.cpp"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
