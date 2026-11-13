"""Tests for the generated Highrim.esp.

An independent reader (written here on purpose, it does not import tools/esp.py) re-parses the file the
generator wrote and checks its structure. The file is verified by parsing only; it has never been loaded
in the game by this test.

Run: python -I tests/test_esp.py
"""
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GENERATOR = os.path.join(ROOT, "tools", "make_esp.py")
COMMITTED_ESP = os.path.join(ROOT, "package", "Data", "Highrim.esp")
COMMITTED_HEADER = os.path.join(ROOT, "src", "Forms.h")
SKYRIM_ESM = os.environ.get(
    "HIGHRIM_SKYRIM_ESM",
    r"C:\Users\Andrew\Documents\My Games\_STEAM\steamapps\common\Skyrim Special Edition\Data\Skyrim.esm",
)

# The documented EDID list (keep in sync with the docstring of tools/make_esp.py).
HOLDS = ["Whiterun", "Eastmarch", "Rift", "Haafingar", "Hjaalmarch", "Reach", "Falkreath", "Pale", "Winterhold"]
EXPECTED_EDIDS = {
    "KYWD": ["HighrimDrug"],
    "MGEF": [
        "HighrimFxRimerushSpeed", "HighrimFxRimerushStamina", "HighrimFxRimerushRegen",
        "HighrimFxVelvetResist", "HighrimFxVelvetSlow", "HighrimFxRegenDown",
        "HighrimAbRegenDown", "HighrimAbStaminaDown", "HighrimAbHealRateDown",
    ],
    "SPEL": [
        "HighrimComedownRimerush", "HighrimComedownVelvetHaze", "HighrimComedownDreamcap",
        "HighrimWithdrawal1", "HighrimWithdrawal2", "HighrimWithdrawal3",
        "HighrimHangover", "HighrimBlurPulse",
    ],
    "BOOK": ["HighrimNote" + hold for hold in HOLDS],
    "ALCH": ["HighrimRimerush", "HighrimVelvetHaze", "HighrimDreamcap"],
    "COBJ": ["HighrimRecipeRimerush", "HighrimRecipeVelvetHaze", "HighrimRecipeDreamcap"],
    "QUST": ["Highrim_MCM"],
}
ALL_EDIDS = {edid for group in EXPECTED_EDIDS.values() for edid in group}

COOKPOT = 0x000A5CB3
GIANT_LICHEN, GLOWING_MUSHROOM, LAVENDER, HANGING_MOSS, SWAMP_FUNGAL_POD = (
    0x0007E8C1, 0x0007EE01, 0x00045C28, 0x00057F91, 0x0007E8B7)
CONFIRMED_INGREDIENTS = {GIANT_LICHEN, GLOWING_MUSHROOM, LAVENDER, HANGING_MOSS, SWAMP_FUNGAL_POD}
RECIPES = {
    "HighrimRecipeRimerush": ("HighrimRimerush", [(GIANT_LICHEN, 2), (GLOWING_MUSHROOM, 1)]),
    "HighrimRecipeVelvetHaze": ("HighrimVelvetHaze", [(LAVENDER, 2), (HANGING_MOSS, 1)]),
    "HighrimRecipeDreamcap": ("HighrimDreamcap", [(GLOWING_MUSHROOM, 2), (SWAMP_FUNGAL_POD, 1)]),
}
# The MCM quest record exactly as the old make_mcm_plugin.py wrote it (header and data), by SHA-256.
MCM_QUEST_RECORD_SHA256 = "c7ad8cacab853ba98ec7e5fabd447a8a7d545c06b321dcfe06a42bce3746733a"
MCM_QUEST_FORMID = 0x01000D62


class Record:
    def __init__(self, rtype, flags, formid, subs, raw, group):
        self.type, self.flags, self.formid, self.subs, self.raw, self.group = rtype, flags, formid, subs, raw, group

    def get(self, tag):
        return [data for t, data in self.subs if t == tag]

    def first(self, tag):
        found = self.get(tag)
        return found[0] if found else None

    @property
    def edid(self):
        raw = self.first("EDID")
        return raw[:-1].decode("ascii") if raw else None


class Plugin:
    def __init__(self, data):
        self.data = data
        self.records = []
        self.groups = []  # (label, group_type)
        self.tes4 = None
        self.masters = []
        self._parse()

    @staticmethod
    def split_subrecords(body):
        subs, pos = [], 0
        while pos < len(body):
            assert pos + 6 <= len(body), "truncated subrecord header"
            tag = body[pos:pos + 4].decode("ascii")
            size = struct.unpack_from("<H", body, pos + 4)[0]
            pos += 6
            assert pos + size <= len(body), f"subrecord {tag} overruns its record"
            subs.append((tag, body[pos:pos + size]))
            pos += size
        assert pos == len(body), "subrecord sizes do not sum to the record size"
        return subs

    def _parse(self):
        data = self.data
        assert data[:4] == b"TES4"
        size, flags, formid = struct.unpack_from("<III", data, 4)
        assert formid == 0
        body = data[24:24 + size]
        self.tes4 = Record("TES4", flags, 0, self.split_subrecords(body), data[:24 + size], None)
        self.masters = [d[:-1].decode("ascii") for t, d in self.tes4.subs if t == "MAST"]
        pos = 24 + size
        while pos < len(data):
            assert data[pos:pos + 4] == b"GRUP", f"expected GRUP at {pos}"
            pos = self._group(pos)
        assert pos == len(data), "file has trailing bytes"

    def _group(self, pos):
        data = self.data
        gsize, label, gtype = struct.unpack_from("<I4sI", data, pos + 4)
        assert gtype == 0, "only top-level groups are expected"
        label = label.decode("ascii")
        self.groups.append((label, gtype))
        end = pos + gsize
        assert end <= len(data)
        pos += 24
        while pos < end:
            rtype = data[pos:pos + 4].decode("ascii")
            assert rtype != "GRUP", "nested groups are not expected"
            dsize, flags, formid = struct.unpack_from("<III", data, pos + 4)
            assert not flags & 0x00040000, "compressed records are not expected"
            body = data[pos + 24:pos + 24 + dsize]
            assert len(body) == dsize
            self.records.append(Record(rtype, flags, formid, self.split_subrecords(body),
                                       data[pos:pos + 24 + dsize], label))
            pos += 24 + dsize
        assert pos == end, "group size does not match its records"
        return pos

    def by_type(self, rtype):
        return [r for r in self.records if r.type == rtype]

    def by_edid(self):
        return {r.edid: r for r in self.records}


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def read_text(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def vanilla_types(path):
    """FormID -> record type for every record of Skyrim.esm (headers only)."""
    data = read_bytes(path)
    out = {}
    stack = []
    pos = 0
    tsize = struct.unpack_from("<I", data, 4)[0]
    pos = 24 + tsize
    ends = []
    while pos < len(data):
        while ends and pos >= ends[-1]:
            ends.pop()
        sig = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        if sig == b"GRUP":
            ends.append(pos + size)
            pos += 24
        else:
            out[struct.unpack_from("<I", data, pos + 12)[0]] = sig.decode("ascii")
            pos += 24 + size
    return out


def run_generator(out_esp, out_header):
    subprocess.run([sys.executable, "-I", GENERATOR, out_esp, out_header], check=True, cwd=tempfile.gettempdir())


class EspTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.esp = os.path.join(cls.tmp.name, "a.esp")
        cls.hdr = os.path.join(cls.tmp.name, "Forms.h")
        run_generator(cls.esp, cls.hdr)
        cls.bytes = read_bytes(cls.esp)
        cls.plugin = Plugin(cls.bytes)
        cls.edid = cls.plugin.by_edid()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    # --- header and structure -------------------------------------------------
    def test_light_plugin_flag_and_masters(self):
        tes4 = self.plugin.tes4
        self.assertTrue(tes4.flags & 0x200, "ESL flag must be set")
        self.assertFalse(tes4.flags & 0x80, "plugin must not be localized (strings are inline)")
        self.assertEqual(self.plugin.masters, ["Skyrim.esm"])

    def test_header_record_count_and_next_object_id(self):
        hedr = self.plugin.tes4.first("HEDR")
        version, count, next_id = struct.unpack("<fII", hedr)
        self.assertAlmostEqual(version, 1.7, places=5)
        self.assertEqual(count, len(self.plugin.records) + len(self.plugin.groups))
        own = [r.formid & 0xFFFFFF for r in self.plugin.records]
        self.assertGreater(next_id, max(own))
        self.assertLessEqual(next_id, 0x1000)

    def test_record_headers_are_24_bytes_and_sizes_add_up(self):
        # Plugin._parse already asserts every subrecord run and group size; here check the totals.
        counted = len(self.plugin.tes4.raw) + 24 * len(self.plugin.groups) + sum(len(r.raw) for r in self.plugin.records)
        self.assertEqual(len(self.bytes), counted)
        for record in self.plugin.records:
            self.assertEqual(len(record.raw), 24 + sum(6 + len(d) for _, d in record.subs))

    def test_group_labels_match_contents(self):
        for record in self.plugin.records:
            self.assertEqual(record.group, record.type)
        labels = [label for label, _ in self.plugin.groups]
        self.assertEqual(len(labels), len(set(labels)), "one group per record type")

    def test_form_ids_unique_and_in_light_range(self):
        ids = [r.formid for r in self.plugin.records]
        self.assertEqual(len(ids), len(set(ids)))
        for formid in ids:
            self.assertEqual(formid >> 24, 1, f"{formid:08X} must use master index 1 (this plugin)")
            self.assertTrue(0x800 <= (formid & 0xFFFFFF) <= 0xFFF, f"{formid:08X} outside light range")

    def test_edid_set_matches_documented_list(self):
        found = {}
        for record in self.plugin.records:
            found.setdefault(record.type, []).append(record.edid)
        self.assertEqual({k: sorted(v) for k, v in found.items()},
                         {k: sorted(v) for k, v in EXPECTED_EDIDS.items()})
        self.assertEqual(len({r.edid for r in self.plugin.records}), len(self.plugin.records))

    def test_references_to_own_records_resolve(self):
        own = {r.formid for r in self.plugin.records}
        checked = 0
        for record in self.plugin.records:
            for tag, data in record.subs:
                if tag in ("EFID", "CNAM", "KWDA", "MDOB") and record.type != "BOOK" and len(data) % 4 == 0:
                    for (value,) in struct.iter_unpack("<I", data):
                        if value >> 24 == 1:
                            self.assertIn(value, own, f"{record.edid} {tag} points at missing {value:08X}")
                            checked += 1
        self.assertGreater(checked, 5)

    # --- MCM quest ------------------------------------------------------------
    def test_mcm_quest_record_is_unchanged(self):
        quest = self.edid["Highrim_MCM"]
        self.assertEqual(quest.formid, MCM_QUEST_FORMID)
        self.assertEqual(hashlib.sha256(quest.raw).hexdigest(), MCM_QUEST_RECORD_SHA256)
        vmad = quest.first("VMAD")
        self.assertIn(b"MCM_ConfigBase", vmad)
        self.assertIn(b"SKI_PlayerLoadGameAlias", vmad)

    # --- COBJ -----------------------------------------------------------------
    def test_recipes(self):
        cobjs = self.plugin.by_type("COBJ")
        self.assertEqual(len(cobjs), 3)
        for cobj in cobjs:
            product, ingredients = RECIPES[cobj.edid]
            cnto = cobj.get("CNTO")
            self.assertEqual(struct.unpack("<I", cobj.first("COCT"))[0], len(cnto))
            self.assertEqual(len(cnto), len(ingredients))
            self.assertEqual(struct.unpack("<I", cobj.first("BNAM"))[0], COOKPOT)
            self.assertEqual(struct.unpack("<I", cobj.first("CNAM"))[0], self.edid[product].formid)
            self.assertEqual(struct.unpack("<H", cobj.first("NAM1"))[0], 1)
            self.assertEqual([struct.unpack("<II", c) for c in cnto], ingredients)
            self.assertFalse(cobj.get("CTDA"), "recipes carry no conditions")
            for item, count in ingredients:
                self.assertIn(item, CONFIRMED_INGREDIENTS)
                self.assertGreater(count, 0)

    # --- ALCH / keyword ---------------------------------------------------------
    def test_drugs_carry_the_dealer_keyword_and_effects(self):
        keyword = self.edid["HighrimDrug"].formid
        for edid in EXPECTED_EDIDS["ALCH"]:
            alch = self.edid[edid]
            kwda = struct.unpack(f"<{len(alch.first('KWDA')) // 4}I", alch.first("KWDA"))
            self.assertEqual(struct.unpack("<I", alch.first("KSIZ"))[0], len(kwda))
            self.assertIn(keyword, kwda)
            efid, efit = alch.get("EFID"), alch.get("EFIT")
            self.assertEqual(len(efid), len(efit))
            self.assertGreaterEqual(len(efid), 2)
            for raw in efit:
                magnitude, area, duration = struct.unpack("<fII", raw)
                self.assertEqual(duration, 120, "high lasts 120 s")
            enit = alch.first("ENIT")
            self.assertEqual(len(enit), 20)
            self.assertTrue(alch.first("FULL").endswith(b"\0"))

    def test_withdrawal_and_hangover_are_abilities_comedowns_are_spells(self):
        for edid in ("HighrimWithdrawal1", "HighrimWithdrawal2", "HighrimWithdrawal3", "HighrimHangover"):
            spit = self.edid[edid].first("SPIT")
            self.assertEqual(len(spit), 36)
            self.assertEqual(struct.unpack_from("<I", spit, 8)[0], 4, f"{edid} is an ability (type 4)")
        for edid in ("HighrimComedownRimerush", "HighrimComedownVelvetHaze", "HighrimComedownDreamcap", "HighrimBlurPulse"):
            spit = self.edid[edid].first("SPIT")
            self.assertEqual(struct.unpack_from("<I", spit, 8)[0], 0, f"{edid} is a spell (type 0)")
            self.assertEqual(struct.unpack_from("<I", spit, 16)[0], 1, "fire and forget")
            self.assertEqual(struct.unpack_from("<I", spit, 20)[0], 0, "self")
        for edid in ("HighrimComedownRimerush", "HighrimComedownVelvetHaze", "HighrimComedownDreamcap"):
            durations = {struct.unpack("<fII", e)[2] for e in self.edid[edid].get("EFIT")}
            self.assertEqual(durations, {60})
        # Tiers strictly escalate: more effects each tier, and the slow-regen magnitude rises.
        counts = [len(self.edid[f"HighrimWithdrawal{n}"].get("EFID")) for n in (1, 2, 3)]
        self.assertEqual(counts, sorted(set(counts)))
        regen_down = self.edid["HighrimAbRegenDown"].formid
        mags = []
        for n in (1, 2, 3):
            spell = self.edid[f"HighrimWithdrawal{n}"]
            for efid, efit in zip(spell.get("EFID"), spell.get("EFIT")):
                if struct.unpack("<I", efid)[0] == regen_down:
                    mags.append(struct.unpack("<f", efit[:4])[0])
        self.assertEqual(mags, sorted(set(mags)))
        self.assertEqual(len(mags), 3)

    # --- BOOK -------------------------------------------------------------------
    def test_nine_notes_with_exact_text(self):
        books = self.plugin.by_type("BOOK")
        self.assertEqual(len(books), 9)
        for hold in HOLDS:
            book = self.edid["HighrimNote" + hold]
            text = book.first("DESC")
            self.assertEqual(text, f"Your things are in the {hold} prison.".encode("ascii") + b"\0")

    # --- determinism, committed outputs -----------------------------------------
    def test_deterministic_across_runs(self):
        second = os.path.join(self.tmp.name, "b.esp")
        second_hdr = os.path.join(self.tmp.name, "b.h")
        run_generator(second, second_hdr)
        self.assertEqual(read_bytes(second), self.bytes)
        self.assertEqual(read_bytes(second_hdr), read_bytes(self.hdr))

    def test_committed_files_are_current(self):
        self.assertEqual(read_bytes(COMMITTED_ESP), self.bytes, "package/Data/Highrim.esp is stale: run tools/make_esp.py")
        # Git may check the header out with CRLF line endings; compare ignoring that.
        self.assertEqual(read_bytes(COMMITTED_HEADER).replace(b"\r\n", b"\n"), read_bytes(self.hdr),
                         "src/Forms.h is stale: run tools/make_esp.py")

    def test_forms_header_matches_the_plugin(self):
        text = read_text(self.hdr)
        found = dict(re.findall(r"constexpr\s+std::uint32_t\s+(\w+)\s*=\s*0x([0-9A-Fa-f]+);", text))
        expected = {r.edid: f"{r.formid & 0xFFFFFF:06X}" for r in self.plugin.records}
        self.assertEqual({k: v.upper() for k, v in found.items()}, expected)
        self.assertIn('"Highrim.esp"', text)

    # --- vanilla references (needs the game's Skyrim.esm, skipped without it) ----
    @unittest.skipUnless(os.path.exists(SKYRIM_ESM), "Skyrim.esm not found")
    def test_every_vanilla_reference_exists_with_the_right_type(self):
        vanilla = vanilla_types(SKYRIM_ESM)
        expectations = {"EFID": "MGEF", "CNTO": "INGR", "BNAM": "KYWD", "KWDA": "KYWD", "MDOB": None}
        seen = 0
        for record in self.plugin.records:
            for tag, data in record.subs:
                if record.type == "QUST" or tag not in expectations:
                    continue
                if tag == "CNTO":
                    ids = [struct.unpack_from("<I", data)[0]]
                elif tag in ("BNAM",) and record.type != "COBJ":
                    continue
                else:
                    ids = [v for (v,) in struct.iter_unpack("<I", data)]
                for formid in ids:
                    if formid >> 24 != 0:
                        continue
                    self.assertIn(formid, vanilla, f"{record.edid} {tag} {formid:08X} not in Skyrim.esm")
                    if expectations[tag]:
                        self.assertEqual(vanilla[formid], expectations[tag], f"{record.edid} {tag} {formid:08X}")
                    seen += 1
        self.assertGreater(seen, 20)


if __name__ == "__main__":
    unittest.main(verbosity=2)
