"""Fails if the plugin calls a CommonLibSSE-NG function that has no address on Skyrim AE / 1.7.x.

Some NG functions are defined as RELOCATION_ID(<SE id>, 0): on AE and 1.7.x (including 1.7.104) the address is
missing, so calling them is a call through a null pointer and crashes the game (Actor::IsInBleedout did exactly
that). This scans the NG sources for such definitions and checks that no src/*.cpp calls one of them by name.
Name matching is deliberately broad (any `->Name(` or `.Name(` call), so it can report a false positive; if it
does, rename or prove the call safe and add it to ALLOWED with the class name and a reason.
"""
import glob
import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALLOWED = {
    # function name -> (class whose method has no AE address, reason). Safe while src never mentions that class.
    "GetSingleton": ("CombatBlackboardManager", "many other classes have GetSingleton; we never use the combat blackboard"),
}

DEF = re.compile(r"\w[\w:<>\*&\s]*\s+(\w+)::(\w+)\([^)]*\)[^{;]*\{[^}]*?RELOCATION_ID\((\d+),\s*(\d+)\)", re.S)


def null_on_ae():
    found = {}
    for path in glob.glob(os.path.join(ROOT, "lib", "commonlibsse-ng", "src", "RE", "**", "*.cpp"), recursive=True):
        with open(path, encoding="utf-8", errors="ignore") as fh:
            text = fh.read()
        for m in DEF.finditer(text):
            cls, fn, se, ae = m.groups()
            if ae == "0":
                found.setdefault(fn, []).append(f"{cls}::{fn} RELOCATION_ID({se}, {ae})")
    return found


def called_names():
    names = set()
    for path in glob.glob(os.path.join(ROOT, "src", "*.cpp")):
        with open(path, encoding="utf-8", errors="ignore") as fh:
            text = fh.read()
        names |= set(re.findall(r"(?:->|\.)([A-Z]\w+)\(", text))
        names |= set(re.findall(r"RE::\w+::([A-Z]\w+)\(", text))
    return names


class NgRelocationTests(unittest.TestCase):
    def test_ng_sources_are_present(self):
        self.assertTrue(null_on_ae(), "expected to find at least the known RELOCATION_ID(x, 0) definitions")

    def test_plugin_calls_no_function_without_an_ae_address(self):
        bad = null_on_ae()
        sources = "".join(
            open(path, encoding="utf-8", errors="ignore").read() for path in glob.glob(os.path.join(ROOT, "src", "*.*"))
        )
        hits = []
        for name in sorted(called_names()):
            if name not in bad:
                continue
            allowed = ALLOWED.get(name)
            if allowed and allowed[0] not in sources:
                continue
            hits.append(f"{name}: {', '.join(bad[name])}")
        self.assertEqual(hits, [], "calls to NG functions with no AE/1.7.x address (null call, crashes the game)")


if __name__ == "__main__":
    unittest.main(verbosity=2)
