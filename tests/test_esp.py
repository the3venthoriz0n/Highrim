"""Tests for the generated Highrim.esp.

An independent reader (written here on purpose, it does not import tools/esp.py) re-parses the file the
generator wrote and checks its structure. The file is verified by parsing only; it has never been loaded
in the game by this test.

Run: python -I tests/test_esp.py
"""
import hashlib
import json
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
CAPITALS = {"Whiterun": "Whiterun", "Eastmarch": "Windhelm", "Rift": "Riften", "Haafingar": "Solitude",
            "Hjaalmarch": "Morthal", "Reach": "Markarth", "Falkreath": "Falkreath", "Pale": "Dawnstar",
            "Winterhold": "Winterhold"}
# (display name, EDID part, actor value) of the 18 skills
SKILLS = [("One-handed", "OneHanded", 6), ("Two-handed", "TwoHanded", 7), ("Archery", "Archery", 8), ("Block", "Block", 9),
          ("Smithing", "Smithing", 10), ("Heavy Armor", "HeavyArmor", 11), ("Light Armor", "LightArmor", 12),
          ("Pickpocket", "Pickpocket", 13), ("Lockpicking", "Lockpicking", 14), ("Sneak", "Sneak", 15),
          ("Alchemy", "Alchemy", 16), ("Speech", "Speech", 17), ("Alteration", "Alteration", 18),
          ("Conjuration", "Conjuration", 19), ("Destruction", "Destruction", 20), ("Illusion", "Illusion", 21),
          ("Restoration", "Restoration", 22), ("Enchanting", "Enchanting", 23)]
SKILL_NAMES = [edid for _, edid, _ in SKILLS]
INVISIBILITY = 0x0003EB3D   # AlchInvisibillity in Skyrim.esm
DRUG_KEYS = ("Rimerush", "VelvetHaze", "Dreamcap")
TIMED_VARIANT_SECONDS = (600, 3600, 10800, 28800, 64800, 129600, 259200)   # Core::kTimedVariantSeconds


def timed_edids():
    """Task 18: the timed Addicted / withdrawal spells: base, then variants V0..V6 (same order as the generator's SPELL_EDIDS)."""
    out = [f"HighrimTimedAddicted{k}" for k in DRUG_KEYS] + [f"HighrimTimedWithdrawal{t}" for t in (1, 2, 3)]
    for v in range(len(TIMED_VARIANT_SECONDS)):
        out += [f"HighrimTimedAddicted{k}V{v}" for k in DRUG_KEYS]
    for v in range(len(TIMED_VARIANT_SECONDS)):
        out += [f"HighrimTimedWithdrawal{t}V{v}" for t in (1, 2, 3)]
    return out


# Skills the Rimerush buffs go to (Task 18: not Alchemy, not Enchanting); the withdrawal penalty stays on all 18.
RUSH_SKILL_AVS = {av for _, _, av in SKILLS if av not in (16, 23)}
EXPECTED_EDIDS = {
    "KYWD": ["HighrimDrug"],
    "MGEF": [
        "HighrimFxRimerushSpeed", "HighrimFxRimerushStamina", "HighrimFxRimerushRegen",
        "HighrimFxVelvetResist", "HighrimFxVelvetSlow", "HighrimFxRegenDown",
        "HighrimFxDetectLife",
        "HighrimAbRegenDown", "HighrimAbStaminaDown", "HighrimAbHealRateDown",
        "HighrimAbSpeedUp", "HighrimAbStaminaUp", "HighrimAbRegenUp", "HighrimAbResistUp", "HighrimAbSlow",
        "HighrimAbLostGear",
        "HighrimFxHighlyAddictive", "HighrimFxVelvetHealRate", "HighrimFxVelvetCarry", "HighrimAbHealRateUp",
        "HighrimAbCarryUp", "HighrimAbAddicted", "HighrimAbShakySpeech", "HighrimAbShakySneak",
        "HighrimFxRushHealth", "HighrimFxRushMagicka", "HighrimAbHealthUp", "HighrimAbMagickaUp",
    ] + ["HighrimFxRushSkill" + name for name in SKILL_NAMES] + ["HighrimAbRushSkill" + name for name in SKILL_NAMES]
    + ["HighrimAbSkill" + name for name in SKILL_NAMES]
    + ["HighrimAbOverdose" + tier for tier in ("Low", "Elevated", "High")]
    + ["HighrimFxAllSkillsUp", "HighrimAbAllSkillsUp", "HighrimAbAllSkillsDown"]
    # Task 18: attack speed, the ravage effects, the timed addiction / withdrawal effects
    + ["HighrimFxRushAttackSpeed", "HighrimAbAttackSpeedUp", "HighrimFxRavageHealth", "HighrimFxRavageMagicka",
       "HighrimFxRavageStamina", "HighrimFxAddicted", "HighrimFxWdRegenDown", "HighrimFxWdStaminaDown",
       "HighrimFxWdHealRateDown", "HighrimFxWdAllSkillsDown"]
    + ["HighrimFxWdSkill" + name for name in SKILL_NAMES],
    "SPEL": [
        "HighrimComedownRimerush", "HighrimComedownVelvetHaze", "HighrimComedownDreamcap",
        "HighrimWithdrawal1", "HighrimWithdrawal2", "HighrimWithdrawal3",
        "HighrimHangover", "HighrimBlurPulse",
    ] + [f"HighrimStack{drug}{n}" for drug in ("Rimerush", "VelvetHaze", "Dreamcap") for n in (1, 2, 3)]
    + ["HighrimLostGear" + hold for hold in HOLDS]
    + ["HighrimShaky", "HighrimAddictedRimerush", "HighrimAddictedVelvetHaze", "HighrimAddictedDreamcap"]
    + ["HighrimOverdose" + tier for tier in ("Low", "Elevated", "High")]
    + ["HighrimRavage"] + timed_edids(),
    "BOOK": ["HighrimNote" + hold for hold in HOLDS],
    "ALCH": ["HighrimRimerush", "HighrimVelvetHaze", "HighrimDreamcap"],
    "COBJ": ["HighrimRecipeRimerush", "HighrimRecipeVelvetHaze", "HighrimRecipeDreamcap"],
    "IMAD": ["HighrimImodVelvet", "HighrimImodComedown", "HighrimImodOverdose"],
    "QUST": ["Highrim_MCM"],
}
ALL_EDIDS = {edid for group in EXPECTED_EDIDS.values() for edid in group}

COOKPOT = 0x000A5CB3
GIANT_LICHEN, GLOWING_MUSHROOM, LAVENDER, HANGING_MOSS, SWAMP_FUNGAL_POD = (
    0x0007E8C1, 0x0007EE01, 0x00045C28, 0x00057F91, 0x0007E8B7)
GOLD001 = 0x0000000F
RECIPES = {  # every recipe: 1000 gold and nothing else
    "HighrimRecipeRimerush": ("HighrimRimerush", [(GOLD001, 1000)]),
    "HighrimRecipeVelvetHaze": ("HighrimVelvetHaze", [(GOLD001, 1000)]),
    "HighrimRecipeDreamcap": ("HighrimDreamcap", [(GOLD001, 1000)]),
}
# The MCM quest record exactly as the old make_mcm_plugin.py wrote it (header and data), by SHA-256.
MCM_QUEST_RECORD_SHA256 = "c7ad8cacab853ba98ec7e5fabd447a8a7d545c06b321dcfe06a42bce3746733a"
MCM_QUEST_FORMID = 0x01000D62


# Local form IDs shipped in Highrim 0.1.0 (commit 593b429). They must never change: saves refer to them.
# Kept here as a literal on purpose, separate from tools/pinned_form_ids.json, so editing one is caught by the other.
RELEASED_0_1_0 = {
    "HighrimDrug": 0x000800,
    "HighrimFxRimerushSpeed": 0x000801,
    "HighrimFxRimerushStamina": 0x000802,
    "HighrimFxRimerushRegen": 0x000803,
    "HighrimFxVelvetResist": 0x000804,
    "HighrimFxVelvetSlow": 0x000805,
    "HighrimFxRegenDown": 0x000806,
    "HighrimAbRegenDown": 0x000807,
    "HighrimAbStaminaDown": 0x000808,
    "HighrimAbHealRateDown": 0x000809,
    "HighrimComedownRimerush": 0x00080A,
    "HighrimComedownVelvetHaze": 0x00080B,
    "HighrimComedownDreamcap": 0x00080C,
    "HighrimWithdrawal1": 0x00080D,
    "HighrimWithdrawal2": 0x00080E,
    "HighrimWithdrawal3": 0x00080F,
    "HighrimHangover": 0x000810,
    "HighrimBlurPulse": 0x000811,
    "HighrimNoteWhiterun": 0x000812,
    "HighrimNoteEastmarch": 0x000813,
    "HighrimNoteRift": 0x000814,
    "HighrimNoteHaafingar": 0x000815,
    "HighrimNoteHjaalmarch": 0x000816,
    "HighrimNoteReach": 0x000817,
    "HighrimNoteFalkreath": 0x000818,
    "HighrimNotePale": 0x000819,
    "HighrimNoteWinterhold": 0x00081A,
    "HighrimRimerush": 0x00081B,
    "HighrimVelvetHaze": 0x00081C,
    "HighrimDreamcap": 0x00081D,
    "HighrimRecipeRimerush": 0x00081E,
    "HighrimRecipeVelvetHaze": 0x00081F,
    "HighrimRecipeDreamcap": 0x000820,
    "Highrim_MCM": 0x000D62,
}
PINNED_JSON = os.path.join(ROOT, "tools", "pinned_form_ids.json")


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


def read_template_imad(key="00031060"):
    """Subrecords of a vanilla IMAD (default ParalyzeMassImod) as stored in tools/vanilla_templates.json."""
    with open(os.path.join(ROOT, "tools", "vanilla_templates.json"), encoding="utf-8") as handle:
        raw = json.load(handle)[key]
    assert raw["type"] == "IMAD"
    return {tag: bytes.fromhex(data) for tag, data in raw["subs"]}


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

    # --- pinned form IDs (0.1.0) ---------------------------------------------------
    def test_pinned_file_matches_the_0_1_0_table(self):
        with open(PINNED_JSON, encoding="utf-8") as handle:
            pinned = {k: int(v, 16) for k, v in json.load(handle)["pinned"].items()}
        self.assertEqual(pinned, RELEASED_0_1_0, "pinned_form_ids.json must keep every 0.1.0 ID unchanged")

    def test_released_records_keep_their_0_1_0_form_ids(self):
        for edid, local in RELEASED_0_1_0.items():
            self.assertIn(edid, self.edid, f"pinned record {edid} is missing from the generated plugin")
            self.assertEqual(self.edid[edid].formid, 0x01000000 | local, f"{edid} moved from {local:#06x}")

    def test_new_records_are_allocated_after_the_highest_pinned_id(self):
        top = max(RELEASED_0_1_0.values())
        new = {r.edid: r.formid & 0xFFFFFF for r in self.plugin.records if r.edid not in RELEASED_0_1_0}
        self.assertTrue(new)
        self.assertTrue(all(local > top for local in new.values()), new)
        self.assertEqual(sorted(new.values()), list(range(top + 1, top + 1 + len(new))), "contiguous, no reuse")

    def test_generator_refuses_a_missing_pinned_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = os.path.join(tmp, "pins.json")
            with open(PINNED_JSON, encoding="utf-8") as handle:
                data = json.load(handle)
            data["pinned"]["HighrimGhostRecord"] = "0x000FF0"
            with open(bad, "w", encoding="utf-8") as handle:
                json.dump(data, handle)
            env = dict(os.environ, HIGHRIM_PINNED_IDS=bad)
            result = subprocess.run([sys.executable, "-I", GENERATOR, os.path.join(tmp, "x.esp"), os.path.join(tmp, "x.h")],
                                    env=env, capture_output=True, text=True, cwd=tmp)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("HighrimGhostRecord", result.stderr)

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
            self.assertEqual(ingredients, [(GOLD001, 1000)], "exactly 1000 gold, no other ingredient")

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
            self.assertGreaterEqual(len(efid), 1)
            for raw in efit:
                magnitude, area, duration = struct.unpack("<fII", raw)
                self.assertEqual(duration, 120, "high lasts 120 s")
            enit = alch.first("ENIT")
            self.assertEqual(len(enit), 20)
            self.assertTrue(alch.first("FULL").endswith(b"\0"))

    def test_dreamcap_potion_has_no_detect_life_effect(self):
        # Reveal life is cast by the plugin (vanilla Detect Life); potion effects never applied it. The pinned-by-use
        # MGEF record stays in the plugin but no potion lists it.
        dreamcap = self.edid["HighrimDreamcap"]
        fx = self.edid["HighrimFxDetectLife"]
        efids = [struct.unpack("<I", raw)[0] for raw in dreamcap.get("EFID")]
        self.assertEqual(len(efids), 2, "the sleeping tree vision effect and the highly addictive descriptor")
        self.assertNotIn(fx.formid, efids)
        for edid in EXPECTED_EDIDS["ALCH"]:
            self.assertNotIn(fx.formid, [struct.unpack("<I", r)[0] for r in self.edid[edid].get("EFID")])

    def test_every_custom_effect_and_spell_has_a_description(self):
        for record in self.plugin.by_type("MGEF"):
            self.assertTrue(record.first("DNAM").strip(bytes(1)), f"{record.edid} has no description")
        for record in self.plugin.by_type("SPEL"):
            text = record.first("DESC").strip(bytes(1)).decode("ascii")
            self.assertGreater(len(text), len(record.first("FULL").strip(bytes(1))), f"{record.edid} description is just its name")
            self.assertNotIn("<", text, f"{record.edid}: tokens must be filled in")
        self.assertIn(b"60", self.edid["HighrimComedownRimerush"].first("DESC"))

    def test_comedown_withdrawal_hangover_names_and_descriptions(self):
        def text(edid, tag):
            return self.edid[edid].first(tag).rstrip(bytes(1)).decode("ascii")
        names = {"HighrimComedownRimerush": "Rimerush Crash", "HighrimComedownVelvetHaze": "Velvet Crash",
                 "HighrimComedownDreamcap": "Dreamcap Crash", "HighrimWithdrawal1": "Withdrawal I: Cravings",
                 "HighrimWithdrawal2": "Withdrawal II: Shakes", "HighrimWithdrawal3": "Withdrawal III: Sickness",
                 "HighrimHangover": "Hangover"}
        for edid, name in names.items():
            self.assertEqual(text(edid, "FULL"), name)
        self.assertEqual(text("HighrimComedownRimerush", "DESC"), "Stamina regeneration 60% slower for 60 seconds.")
        self.assertEqual(text("HighrimComedownVelvetHaze", "DESC"), "Stamina regeneration 40% slower for 60 seconds.")
        self.assertEqual(text("HighrimComedownDreamcap", "DESC"), "Your vision blurs for 60 seconds.")
        self.assertEqual(text("HighrimWithdrawal1", "DESC"), "Stamina regeneration 20% slower. All skills 10 lower in withdrawal.")
        self.assertIn("Maximum stamina 25 lower", text("HighrimWithdrawal2", "DESC"))
        self.assertIn("Health regeneration 50% slower", text("HighrimWithdrawal3", "DESC"))
        self.assertIn("Lasts one game day", text("HighrimHangover", "DESC"))
        for edid in names:
            self.assertNotIn("-", text(edid, "DESC").replace("Withdrawal", ""), f"{edid}: no raw negative numbers")

    def test_lost_gear_abilities(self):
        for hold in HOLDS:
            spell = self.edid["HighrimLostGear" + hold]
            self.assertEqual(spell.first("FULL"), f"Lost Gear: {CAPITALS[hold]}?".encode("ascii") + b"\0")
            self.assertEqual(spell.first("DESC"), f"The guards in {CAPITALS[hold]} may have your things.".encode("ascii") + b"\0")
            self.assertNotIn(b"rison", spell.first("FULL") + spell.first("DESC"))
            self.assertEqual(struct.unpack_from("<I", spell.first("SPIT"), 8)[0], 4, "ability")
            self.assertEqual(len(spell.get("EFID")), 1)

    def test_comedown_imad_is_black_and_white_without_blur(self):
        mine = self.edid["HighrimImodComedown"]
        vanilla = read_template_imad()
        self.assertEqual(mine.type, "IMAD")
        for tag in ("BNAM", "VNAM", "RNAM", "SNAM", "UNAM", "NAM1", "NAM2", "WNAM", "XNAM", "YNAM", "NAM4"):
            data = mine.first(tag)
            floats = struct.unpack(f"<{len(data) // 4}f", data)
            self.assertTrue(all(v == 0.0 for v in floats[1::2]), f"{tag} has blur left")
        for tag in ("TNAM", "NAM3"):
            floats = struct.unpack("<25f", mine.first(tag))
            self.assertTrue(all(v == 0.0 for v in floats[4::5]), f"{tag} still tints")
        for tag in ("\x11IAD", "QIAD"):
            data = mine.first(tag)
            self.assertTrue(all(v == 0.0 for v in struct.unpack(f"<{len(data) // 4}f", data)[1::2]), "saturation not zero")
            self.assertEqual(len(data), len(vanilla[tag]))
        self.assertAlmostEqual(struct.unpack_from("<f", mine.first("DNAM"), 4)[0], 60.0)

    # --- Task 16: overdose risk -----------------------------------------------------------------
    def test_overdose_risk_abilities(self):
        tiers = (("Low", "Overdose Risk: Low", "You are handling it, for now."),
                 ("Elevated", "Overdose Risk: Elevated", "One more dose could knock you out."),
                 ("High", "Overdose Risk: High", "Another dose may put you on the floor."))
        ids = set()
        for key, name, desc in tiers:
            spell = self.edid["HighrimOverdose" + key]
            self.assertEqual(spell.first("FULL"), name.encode("ascii") + bytes(1))
            self.assertEqual(spell.first("DESC"), desc.encode("ascii") + bytes(1))
            self.assertEqual(struct.unpack_from("<I", spell.first("SPIT"), 8)[0], 4, "ability, lasts until removed")
            fx = self.effects_of("HighrimOverdose" + key)
            self.assertEqual(len(fx), 1)
            self.assertEqual(fx[0][0], self.edid["HighrimAbOverdose" + key].formid)
            self.assertEqual(fx[0][1][0], 0.0, "no gameplay effect: magnitude 0")
            mgef = self.edid["HighrimAbOverdose" + key]
            self.assertEqual(struct.unpack_from("<I", mgef.first("DATA"), 0)[0] & 0x5, 0, "not hostile or detrimental")
            self.assertEqual(struct.unpack_from("<I", mgef.first("DATA"), 68)[0], 32, "carry weight, magnitude 0")
            self.assertEqual(mgef.first("FULL"), name.encode("ascii") + bytes(1))
            self.assertEqual(mgef.first("DNAM"), desc.encode("ascii") + bytes(1))
            ids.add(spell.formid)
        self.assertEqual(len(ids), 3)

    def test_overdose_texts_match_the_plugin_core(self):
        # Core::RiskTierName / RiskTierDescription in src/Core.cpp are the same strings
        core = read_text(os.path.join(ROOT, "src", "Core.cpp"))
        for key in ("Low", "Elevated", "High"):
            spell = self.edid["HighrimOverdose" + key]
            for tag in ("FULL", "DESC"):
                text = spell.first(tag)[:-1].decode("ascii")
                self.assertIn(f'"{text}"', core, f"{key} {tag} text is not in Core.cpp")

    def test_overdose_imad_has_no_blur_a_visible_tint_and_a_short_run(self):
        mine = self.edid["HighrimImodOverdose"]
        vanilla = read_template_imad("0010C445")
        self.assertEqual(mine.type, "IMAD")
        blur_tags = ("BNAM", "VNAM", "RNAM", "SNAM", "UNAM", "NAM1", "NAM2", "WNAM", "XNAM", "YNAM", "NAM4")
        for tag in blur_tags:
            data = mine.first(tag)
            floats = struct.unpack(f"<{len(data) // 4}f", data)
            self.assertTrue(all(v == 0.0 for v in floats[1::2]), f"{tag} still blurs")
            self.assertEqual(len(data), len(vanilla[tag]), f"{tag} key count must be unchanged")
        self.assertGreater(max(struct.unpack(f"<{len(vanilla['BNAM']) // 4}f", vanilla["BNAM"])[1::2]), 0, "the source had blur")
        tint = struct.unpack("<15f", mine.first("TNAM"))
        source = struct.unpack("<15f", vanilla["TNAM"])
        self.assertAlmostEqual(max(tint[4::5]), 0.35, places=5)
        self.assertLess(max(source[4::5]), 0.35, "the vanilla tint is fainter")
        self.assertEqual(tint[0::5], source[0::5], "tint key times unchanged")
        self.assertEqual([tint[i] for i in range(15) if i % 5 not in (4,)], [source[i] for i in range(15) if i % 5 not in (4,)],
                         "tint colours unchanged")
        self.assertAlmostEqual(struct.unpack_from("<f", mine.first("DNAM"), 4)[0], 4.0)
        for tag, data in vanilla.items():
            if tag not in ("EDID", "TNAM", "DNAM") + blur_tags:
                self.assertEqual(mine.first(tag), data, f"{tag} must match the source")

    def test_detrimental_effects_are_not_flagged_hostile_or_detrimental(self):
        # Cure Disease and other dispels: our comedown, withdrawal and hangover effects carry no Hostile (0x1) or
        # Detrimental (0x4) flag and no harmful keyword; they are negative-magnitude Fortify style effects.
        for edid in ("HighrimFxRegenDown", "HighrimAbRegenDown", "HighrimAbStaminaDown", "HighrimAbHealRateDown", "HighrimAbSlow"):
            flags = struct.unpack_from("<I", self.edid[edid].first("DATA"), 0)[0]
            self.assertEqual(flags & 0x5, 0, f"{edid} flags {flags:#x}")
            self.assertFalse(self.edid[edid].get("KWDA"), edid)
        for edid in ("HighrimComedownRimerush", "HighrimComedownVelvetHaze", "HighrimWithdrawal1", "HighrimWithdrawal2",
                     "HighrimWithdrawal3", "HighrimHangover"):
            for efit in self.edid[edid].get("EFIT"):
                if edid.startswith("HighrimComedown") or edid.startswith("HighrimWith"):
                    pass
            mags = [struct.unpack("<f", e[:4])[0] for e in self.edid[edid].get("EFIT")]
            self.assertTrue(all(m < 0 for m in mags), f"{edid} magnitudes must be negative: {mags}")
        self.assertEqual(struct.unpack_from("<I", self.edid["HighrimAbHealRateDown"].first("DATA"), 68)[0], 0x9B)

    def test_velvet_haze_imad_has_no_blur_but_keeps_colour(self):
        mine = self.edid["HighrimImodVelvet"]
        self.assertEqual(mine.type, "IMAD")
        self.assertEqual(mine.first("DNAM")[:8], read_template_imad()["DNAM"][:8])
        vanilla = read_template_imad()
        for tag in ("BNAM", "VNAM", "RNAM", "SNAM", "UNAM", "NAM1", "NAM2", "WNAM", "XNAM", "YNAM", "NAM4"):
            data = mine.first(tag)
            floats = struct.unpack(f"<{len(data) // 4}f", data)
            self.assertTrue(all(v == 0.0 for v in floats[1::2]), f"{tag} still has non-zero values")
            self.assertEqual(len(data), len(vanilla[tag]), f"{tag} key count must be unchanged")
        self.assertGreater(max(struct.unpack(f"<{len(vanilla['BNAM']) // 4}f", vanilla["BNAM"])[1::2]), 0, "source had blur")
        for tag, data in vanilla.items():
            if tag not in ("EDID", "BNAM", "VNAM", "RNAM", "SNAM", "UNAM", "NAM1", "NAM2", "WNAM", "XNAM", "YNAM", "NAM4"):
                self.assertEqual(mine.first(tag), data, f"{tag} (colour, tint, saturation) must match the source")

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
                    mags.append(abs(struct.unpack("<f", efit[:4])[0]))
        self.assertEqual(mags, sorted(set(mags)))
        self.assertEqual(len(mags), 3)

    def test_stack_abilities_escalate_and_show_each_effect_once(self):
        names = set()
        for drug in ("Rimerush", "VelvetHaze", "Dreamcap"):
            levels = [self.edid[f"HighrimStack{drug}{n}"] for n in (1, 2, 3)]
            per_level = []
            for spell in levels:
                spit = spell.first("SPIT")
                self.assertEqual(struct.unpack_from("<I", spit, 8)[0], 4, f"{spell.edid} is an ability")
                efids = [struct.unpack("<I", raw)[0] for raw in spell.get("EFID")]
                self.assertEqual(len(set(efids)), len(efids), f"{spell.edid} lists no effect twice")
                self.assertGreaterEqual(len(efids), 2)
                for efit in spell.get("EFIT"):
                    self.assertEqual(struct.unpack("<fII", efit)[2], 0, "ability effects have no duration")
                mags = {fid: abs(struct.unpack("<f", efit[:4])[0]) for fid, efit in zip(efids, spell.get("EFIT"))}
                per_level.append(mags)
                name = spell.first("FULL")
                self.assertNotIn(name, names, "distinct names")
                names.add(name)
            self.assertEqual(set(per_level[0]), set(per_level[1]))
            self.assertEqual(set(per_level[0]), set(per_level[2]))
            for fid in per_level[0]:
                self.assertLess(per_level[0][fid], per_level[1][fid], "level 2 stronger than 1")
                self.assertLess(per_level[1][fid], per_level[2][fid], "level 3 stronger than 2")
        # The new ability effects are constant-effect clones (cast type 0) with distinct actor values / names.
        for edid in ("HighrimAbSpeedUp", "HighrimAbStaminaUp", "HighrimAbRegenUp", "HighrimAbHealRateUp", "HighrimAbCarryUp",
                     "HighrimAbSlow"):
            data = self.edid[edid].first("DATA")
            self.assertEqual(struct.unpack_from("<I", data, 64)[0], 34, "peak value modifier")
            self.assertTrue(self.edid[edid].first("DNAM").strip(bytes(1)), f"{edid} has its description")
        av = lambda e: struct.unpack_from("<I", self.edid[e].first("DATA"), 68)[0]
        self.assertEqual(av("HighrimAbSpeedUp"), 30)
        self.assertEqual(av("HighrimAbRegenUp"), 0x9D)
        self.assertEqual(av("HighrimAbHealRateUp"), 155)
        self.assertEqual(av("HighrimAbCarryUp"), 32)
        for n, (regen, carry) in enumerate(((100, 50), (200, 100), (300, 150)), 1):
            spell = self.edid[f"HighrimStackVelvetHaze{n}"]
            mags = {struct.unpack("<I", e)[0]: struct.unpack("<f", f[:4])[0] for e, f in zip(spell.get("EFID"), spell.get("EFIT"))}
            self.assertEqual(mags[self.edid["HighrimAbHealRateUp"].formid], regen)
            self.assertEqual(mags[self.edid["HighrimAbCarryUp"].formid], carry)
            self.assertNotIn(self.edid["HighrimAbResistUp"].formid, mags, "no armour in the stacks any more")
            self.assertEqual(len(mags), 3, "no invisibility in the stacks")

    def test_potion_base_magnitudes_were_raised(self):
        def mags(edid):
            return [struct.unpack("<f", e[:4])[0] for e in self.edid[edid].get("EFIT")]
        # Task 15 / 18: Rimerush is +10 Health, Magicka, Stamina and 16 skills, +20% attack speed (magnitude 1.2, the whole
        # Weapon Speed Multiplier as the vanilla perks write it), plus the highly addictive descriptor.
        # bars, descriptor (mag = the buff), attack speed, 16 hidden skills, highly addictive
        expected = [10.0] * 4 + [struct.unpack("<f", struct.pack("<f", 1.2))[0]] + [10.0] * 16 + [0.0]
        self.assertEqual(mags("HighrimRimerush"), expected)
        self.assertEqual(mags("HighrimVelvetHaze"), [27.0, 500.0, 150.0, 0.0, 0.0])  # slow, regen, carry, invisible, descriptor

    # --- Task 14 -----------------------------------------------------------------------------
    def effects_of(self, edid):
        rec = self.edid[edid]
        return [(struct.unpack("<I", a)[0], struct.unpack("<fII", b)) for a, b in zip(rec.get("EFID"), rec.get("EFIT"))]

    def test_withdrawal_lowers_all_18_skills_by_tier(self):
        by_id = {self.edid["HighrimAbSkill" + edid].formid: (name, av) for name, edid, av in SKILLS}
        self.assertEqual(len(by_id), 18)
        for tier, penalty in ((1, -10), (2, -20), (3, -30)):
            fx = self.effects_of(f"HighrimWithdrawal{tier}")
            skills = [(by_id[f], e) for f, e in fx if f in by_id]
            self.assertEqual(sorted(n for (n, _), _ in skills), sorted(n for n, _, _ in SKILLS), "each skill once")
            for (name, av), (mag, area, dur) in skills:
                self.assertEqual(mag, penalty, f"tier {tier} {name}")
                self.assertEqual(dur, 0)
            self.assertEqual(len({f for f, _ in fx}), len(fx), "no effect listed twice")
            self.assertIn(self.edid["HighrimAbRegenDown"].formid, [f for f, _ in fx])
        tier3 = [f for f, _ in self.effects_of("HighrimWithdrawal3")]
        self.assertIn(self.edid["HighrimAbStaminaDown"].formid, tier3)
        self.assertIn(self.edid["HighrimAbHealRateDown"].formid, tier3)
        for name, edid, av in SKILLS:
            mgef = self.edid["HighrimAbSkill" + edid]
            self.assertEqual(struct.unpack_from("<I", mgef.first("DATA"), 68)[0], av, name)
            self.assertEqual(struct.unpack_from("<I", mgef.first("DATA"), 0)[0] & 0x5, 0, "not hostile or detrimental")
            self.assertIn(name.encode("ascii"), mgef.first("DNAM"))
            self.assertIn(b"withdrawal", mgef.first("DNAM"))
        for name, edid, av in SKILLS:   # Task 18: the timed withdrawal has its own hidden effect per skill, same values
            mgef = self.edid["HighrimFxWdSkill" + edid]
            self.assertEqual(struct.unpack_from("<I", mgef.first("DATA"), 68)[0], av, name)
            self.assertEqual(struct.unpack_from("<I", mgef.first("DATA"), 0)[0] & 0x5, 0, "not hostile or detrimental")
        text = self.edid["HighrimWithdrawal2"].first("DESC").rstrip(bytes(1)).decode("ascii")
        self.assertIn("All skills 20 lower in withdrawal.", text)

    def test_shaky_and_suspicious_zeroes_speech_and_sneak(self):
        spell = self.edid["HighrimShaky"]
        self.assertEqual(spell.first("FULL"), b"Shaky and Suspicious" + bytes(1))
        self.assertEqual(spell.first("DESC"), b"Speech and Sneak are 0 while high." + bytes(1))
        self.assertEqual(struct.unpack_from("<I", spell.first("SPIT"), 8)[0], 4, "ability")
        fx = self.effects_of("HighrimShaky")
        self.assertEqual(len(fx), 2)
        by_formid = {r.formid: r for r in self.plugin.records}
        avs = {struct.unpack_from("<I", by_formid[f].first("DATA"), 68)[0] for f, _ in fx}
        self.assertEqual(avs, {17, 15}, "Speechcraft and Sneak")
        for _, (mag, area, dur) in fx:
            self.assertLessEqual(mag, -999)
            self.assertEqual(dur, 0)

    def test_rimerush_potion_buffs_bars_and_all_skills(self):
        by_id = {r.formid: r for r in self.plugin.records}
        fx = self.effects_of("HighrimRimerush")
        self.assertEqual(len(fx), 22, "3 bars, descriptor, attack speed, 16 hidden skills, highly addictive")
        self.assertEqual(len({f for f, _ in fx}), 22, "each effect once")
        avs = {}
        for f, (mag, area, dur) in fx[:-1]:
            if f == self.edid["HighrimFxAllSkillsUp"].formid:
                continue
            data = by_id[f].first("DATA")
            self.assertEqual(struct.unpack_from("<I", data, 64)[0], 34, "value modifier")
            if f == self.edid["HighrimFxRushAttackSpeed"].formid:
                self.assertAlmostEqual(mag, 1.2, places=5)
                self.assertEqual(dur, 120)
                self.assertEqual(struct.unpack_from("<I", data, 68)[0], 85, "Weapon Speed Multiplier")
                continue
            self.assertEqual((mag, dur), (10.0, 120))
            self.assertTrue(by_id[f].first("FULL").strip(bytes(1)) and by_id[f].first("DNAM").strip(bytes(1)))
            avs[struct.unpack_from("<I", data, 68)[0]] = by_id[f].edid
        self.assertEqual(set(avs), {24, 25, 26} | RUSH_SKILL_AVS, "Health, Magicka, Stamina and 16 skills (Speech 17 too)")
        self.assertEqual(len(RUSH_SKILL_AVS), 16)
        self.assertNotIn(16, avs, "no Alchemy")
        self.assertNotIn(23, avs, "no Enchanting")
        for edid in ("HighrimFxRushSkillAlchemy", "HighrimFxRushSkillEnchanting", "HighrimAbRushSkillAlchemy",
                     "HighrimAbRushSkillEnchanting"):
            self.assertIn(edid, self.edid, "the provisional record stays in the file, unused")
            self.assertNotIn(self.edid[edid].formid, {f for f, _ in fx})
        self.assertEqual(self.effects_of("HighrimRimerush")[-1][0], self.edid["HighrimFxHighlyAddictive"].formid)
        old = {self.edid[e].formid for e in ("HighrimFxRimerushSpeed", "HighrimFxRimerushRegen")}
        self.assertFalse(old & {f for f, _ in fx}, "movement speed and regen are gone from the potion")
        for e in ("HighrimFxRimerushSpeed", "HighrimFxRimerushRegen", "HighrimAbSpeedUp", "HighrimAbRegenUp"):
            self.assertIn(e, self.edid, "pinned record stays")

    def test_rimerush_stack_levels_are_5_per_level_on_bars_and_all_skills(self):
        by_id = {r.formid: r for r in self.plugin.records}
        for n in (1, 2, 3):
            fx = self.effects_of(f"HighrimStackRimerush{n}")
            self.assertEqual(len(fx), 21, "3 bars, descriptor, attack speed, 16 hidden skills")
            avs = set()
            for f, (mag, area, dur) in fx:
                self.assertEqual(dur, 0)
                if f == self.edid["HighrimAbAttackSpeedUp"].formid:
                    self.assertAlmostEqual(mag, 0.05 * n, places=5, msg="+5% per level, added to the potion's multiplier")
                    self.assertEqual(struct.unpack_from("<I", by_id[f].first("DATA"), 68)[0], 85)
                    continue
                self.assertEqual(mag, 5.0 * n)
                if f == self.edid["HighrimAbAllSkillsUp"].formid:
                    continue
                avs.add(struct.unpack_from("<I", by_id[f].first("DATA"), 68)[0])
            self.assertEqual(avs, {24, 25, 26} | RUSH_SKILL_AVS)
            desc = self.edid[f"HighrimStackRimerush{n}"].first("DESC").decode("ascii")
            self.assertIn(f"All skills except Alchemy and Enchanting +{5 * n}.", desc)
            self.assertIn(f"Weapon attacks {5 * n}% faster.", desc)

    # --- Task 17: dense effect lists ------------------------------------------------------------
    HIDE_IN_UI = 0x00008000   # EffectSetting::EffectSettingData::Flag::kHideInUI (1 << 15); read from Skyrim.esm, see below

    def mgef_flags(self, edid):
        return struct.unpack_from("<I", self.edid[edid].first("DATA"), 0)[0]

    def per_skill_mgefs(self):
        return (["HighrimFxRushSkill" + n for n in SKILL_NAMES] + ["HighrimAbRushSkill" + n for n in SKILL_NAMES]
                + ["HighrimAbSkill" + n for n in SKILL_NAMES] + ["HighrimFxWdSkill" + n for n in SKILL_NAMES])

    def test_hide_in_ui_is_set_on_exactly_the_per_skill_effects(self):
        hidden = {r.edid for r in self.plugin.by_type("MGEF") if struct.unpack_from("<I", r.first("DATA"), 0)[0] & self.HIDE_IN_UI}
        # The constant-effect (ability) template AbFortifyStamina already has Hide in UI set, so every HighrimAb* effect is
        # hidden except the two ability descriptors, whose flag the generator clears. Of the potion effects (Fx) exactly
        # the 18 per-skill ones are flagged.
        descriptors = {"HighrimAbAllSkillsUp", "HighrimAbAllSkillsDown", "HighrimAbAttackSpeedUp"}
        ability_effects = {r.edid for r in self.plugin.by_type("MGEF") if r.edid.startswith("HighrimAb")} - descriptors
        # (HighrimFxVelvetSlow is cloned from the vanilla sap slow effect, which has the flag too: not touched by Task 17.)
        timed_hidden = {"HighrimFxWdRegenDown", "HighrimFxWdStaminaDown", "HighrimFxWdHealRateDown"}   # Task 18: like their abilities
        self.assertEqual(hidden, set(self.per_skill_mgefs()) | ability_effects | timed_hidden | {"HighrimFxVelvetSlow"})
        self.assertEqual({e for e in hidden if e.startswith("HighrimFx")},
                         {"HighrimFxRushSkill" + n for n in SKILL_NAMES} | {"HighrimFxWdSkill" + n for n in SKILL_NAMES}
                         | timed_hidden | {"HighrimFxVelvetSlow"})
        self.assertTrue(set(self.per_skill_mgefs()) <= hidden and len(self.per_skill_mgefs()) == 72)
        for edid in ("HighrimFxAllSkillsUp", "HighrimAbAllSkillsUp",
                     "HighrimAbAllSkillsDown", "HighrimFxHighlyAddictive", "HighrimFxRushHealth",
                     "HighrimFxRushAttackSpeed", "HighrimAbAttackSpeedUp", "HighrimFxAddicted", "HighrimFxWdAllSkillsDown",
                     "HighrimFxRavageHealth", "HighrimFxRavageMagicka", "HighrimFxRavageStamina"):
            self.assertFalse(self.mgef_flags(edid) & self.HIDE_IN_UI, edid)

    @unittest.skipUnless(os.path.exists(SKYRIM_ESM), "Skyrim.esm not found")
    def test_hide_in_ui_flag_value_matches_vanilla(self):
        # Vanilla effects that are hidden in the UI (MS04RewardEffectNoDisplay, AbVampireDamageHealth) have bit 15 set;
        # bit 18 (0x40000, the value the task brief guessed) is only set on Bound weapon effects.
        import zlib
        data = read_bytes(SKYRIM_ESM)
        pos = 24 + struct.unpack_from("<I", data, 4)[0]
        found = {}
        while pos < len(data):
            sig = data[pos:pos + 4]
            size = struct.unpack_from("<I", data, pos + 4)[0]
            if sig == b"GRUP":
                pos += 24
                continue
            if sig == b"MGEF":
                flags = struct.unpack_from("<I", data, pos + 8)[0]
                body = data[pos + 24:pos + 24 + size]
                if flags & 0x40000:
                    body = zlib.decompress(body[4:])
                subs = dict(Plugin.split_subrecords(body))
                if subs["EDID"][:-1] in (b"MS04RewardEffectNoDisplay", b"AbVampireDamageHealth", b"BoundSwordFFSelf"):
                    found[subs["EDID"][:-1].decode()] = struct.unpack_from("<I", subs["DATA"], 0)[0]
            pos += 24 + size
        self.assertTrue(found["MS04RewardEffectNoDisplay"] & self.HIDE_IN_UI)
        self.assertTrue(found["AbVampireDamageHealth"] & self.HIDE_IN_UI)
        self.assertFalse(found["BoundSwordFFSelf"] & self.HIDE_IN_UI)
        self.assertTrue(found["BoundSwordFFSelf"] & 0x40000)

    def test_descriptors_exist_once_and_have_no_gameplay_effect(self):
        up = b"All skills except Alchemy and Enchanting +<mag>."
        down = b"All skills changed by <mag> in withdrawal."
        for edid, name, text in (("HighrimFxAllSkillsUp", b"All Skills Boosted", up),
                                 ("HighrimAbAllSkillsUp", b"All Skills Boosted", up),
                                 ("HighrimAbAllSkillsDown", b"All Skills Reduced", down),
                                 ("HighrimFxWdAllSkillsDown", b"All Skills Reduced", down)):
            rec = self.edid[edid]
            self.assertEqual(rec.first("FULL"), name + bytes(1))
            self.assertEqual(rec.first("DNAM"), text + bytes(1))
            av = struct.unpack_from("<I", rec.first("DATA"), 68)[0]
            self.assertEqual(av, 77, "Variable10: no vanilla magic effect uses it, no skill, bar or stat")
        self.assertEqual(self.mgef_flags("HighrimAbAllSkillsDown") & 0x5, 0, "not hostile or detrimental")

    def visible(self, edid):
        by_id = {r.formid: r for r in self.plugin.records}
        out = []
        for f, _ in self.effects_of(edid):
            rec = by_id.get(f)
            hidden = rec is not None and struct.unpack_from("<I", rec.first("DATA"), 0)[0] & self.HIDE_IN_UI
            if not hidden:
                out.append(f)
        return out

    def test_descriptor_is_listed_once_and_visible_lists_are_short(self):
        fx = lambda e: self.edid[e].formid
        self.assertEqual(self.visible("HighrimRimerush"), [fx("HighrimFxRushHealth"), fx("HighrimFxRushMagicka"),
                         fx("HighrimFxRimerushStamina"), fx("HighrimFxAllSkillsUp"), fx("HighrimFxRushAttackSpeed"),
                         fx("HighrimFxHighlyAddictive")])
        self.assertEqual([m for f, (m, _, _) in self.effects_of("HighrimRimerush") if f == fx("HighrimFxAllSkillsUp")], [10.0])
        for n in (1, 2, 3):
            self.assertEqual([f for f, _ in self.effects_of(f"HighrimStackRimerush{n}")].count(fx("HighrimAbAllSkillsUp")), 1)
            self.assertEqual(self.visible(f"HighrimStackRimerush{n}"), [fx("HighrimAbAllSkillsUp"), fx("HighrimAbAttackSpeedUp")])
            down = [m for f, (m, _, _) in self.effects_of(f"HighrimWithdrawal{n}") if f == fx("HighrimAbAllSkillsDown")]
            self.assertEqual(down, [-10.0 * n])
            self.assertIn(fx("HighrimAbAllSkillsDown"), self.visible(f"HighrimWithdrawal{n}"))
            self.assertEqual(len(self.visible(f"HighrimWithdrawal{n}")), 1, "ability effects other than the descriptor keep the template's Hide in UI")
        for rec in self.plugin.by_type("ALCH") + self.plugin.by_type("SPEL"):
            self.assertLessEqual(len(self.visible(rec.edid)), 6, f"{rec.edid} shows too many effects")

    def test_shaky_is_not_on_the_rimerush_potion_and_still_on_the_others(self):
        # The plugin (Core::IsShakyHigh) decides when the ability is applied; the potions carry nothing of it.
        shaky = {self.edid["HighrimAbShakySpeech"].formid, self.edid["HighrimAbShakySneak"].formid}
        for edid in EXPECTED_EDIDS["ALCH"]:
            self.assertFalse(shaky & {f for f, _ in self.effects_of(edid)})

    def test_addicted_abilities(self):
        for key, name in (("Rimerush", "Rimerush"), ("VelvetHaze", "Velvet Haze"), ("Dreamcap", "Dreamcap")):
            spell = self.edid["HighrimAddicted" + key]
            self.assertEqual(spell.first("FULL"), f"Addicted: {name}".encode("ascii") + bytes(1))
            self.assertEqual(spell.first("DESC"), f"You depend on {name}.".encode("ascii") + bytes(1))
            self.assertEqual(struct.unpack_from("<I", spell.first("SPIT"), 8)[0], 4, "ability")
            self.assertEqual(len(spell.get("EFID")), 1)

    def test_every_drug_is_listed_as_highly_addictive_once(self):
        fx = self.edid["HighrimFxHighlyAddictive"]
        self.assertEqual(fx.first("FULL"), b"Highly Addictive" + bytes(1))
        self.assertEqual(fx.first("DNAM"), b"Highly addictive: withdrawal follows when you stop." + bytes(1))
        for edid in EXPECTED_EDIDS["ALCH"]:
            listed = [f for f, _ in self.effects_of(edid)]
            self.assertEqual(listed.count(fx.formid), 1, edid)
            self.assertEqual(len(listed), len(set(listed)), f"{edid} lists no effect twice")
            self.assertEqual([m for f, (m, _, _) in self.effects_of(edid) if f == fx.formid], [0.0])
        self.assertEqual(self.edid["HighrimVelvetHaze"].first("FULL"), b"Velvet Haze" + bytes(1), "items are not renamed")

    def test_velvet_haze_effects(self):
        velvet = self.effects_of("HighrimVelvetHaze")
        self.assertEqual([f for f, _ in velvet], [
            self.edid["HighrimFxVelvetSlow"].formid, self.edid["HighrimFxVelvetHealRate"].formid,
            self.edid["HighrimFxVelvetCarry"].formid, INVISIBILITY, self.edid["HighrimFxHighlyAddictive"].formid])
        self.assertNotIn(self.edid["HighrimFxVelvetResist"].formid, [f for f, _ in velvet], "no armour effect any more")
        self.assertIn("HighrimFxVelvetResist", self.edid, "the pinned record stays")
        self.assertEqual({d for _, (_, _, d) in velvet}, {120})
        regen = struct.unpack_from("<I", self.edid["HighrimFxVelvetHealRate"].first("DATA"), 68)[0]
        carry = struct.unpack_from("<I", self.edid["HighrimFxVelvetCarry"].first("DATA"), 68)[0]
        self.assertEqual((regen, carry), (155, 32))
        for edid in ("HighrimFxVelvetHealRate", "HighrimFxVelvetCarry", "HighrimFxHighlyAddictive"):
            self.assertTrue(self.edid[edid].first("DNAM").strip(bytes(1)))

    # --- Task 18 --------------------------------------------------------------------------------
    def test_texts_are_condensed(self):
        # one short sentence per effect: no name or description is long (the unused Detect Life text is the longest)
        for record in self.plugin.by_type("MGEF"):
            text = record.first("DNAM").strip(bytes(1)).decode("ascii")
            self.assertLessEqual(len(text), 80, f"{record.edid}: {text}")
            if record.edid != "HighrimFxDetectLife":
                self.assertLessEqual(len(text), 66, f"{record.edid}: {text}")
        for record in self.plugin.by_type("SPEL"):
            if len(record.get("EFID")) == 1:
                text = record.first("DESC").strip(bytes(1)).decode("ascii")
                self.assertLessEqual(len(text), 66, f"{record.edid}: {text}")
        for old in (b"while you are", b"is increased by", b"points"):
            for record in self.plugin.by_type("MGEF"):
                self.assertNotIn(old, record.first("DNAM").lower(), record.edid)

    def test_attack_speed_effects(self):
        fx, ab = self.edid["HighrimFxRushAttackSpeed"], self.edid["HighrimAbAttackSpeedUp"]
        for rec, cast in ((fx, None), (ab, 0)):
            self.assertEqual(rec.first("FULL"), b"Faster Attacks" + bytes(1))
            self.assertEqual(struct.unpack_from("<I", rec.first("DATA"), 64)[0], 34, "peak value modifier, so modifiers add up")
            self.assertEqual(struct.unpack_from("<I", rec.first("DATA"), 68)[0], 85, "Weapon Speed Multiplier")
            self.assertEqual(self.mgef_flags(rec.edid) & 0x5, 0, "not hostile or detrimental")
        self.assertEqual(fx.first("DNAM"), b"Weapon attacks 20% faster for <dur> seconds." + bytes(1))
        self.assertEqual(ab.first("DNAM"), b"Weapon attacks are faster." + bytes(1))
        self.assertEqual(self.edid["HighrimStackRimerush2"].first("DESC").count(b"Weapon attacks 10% faster."), 1)

    def test_no_stimulant_screen_effect_constant_and_comedown_look_stays(self):
        header = read_text(self.hdr)
        self.assertNotIn("kImodStimulant", header)
        self.assertIn("HighrimImodComedown", self.edid)

    def test_ravage_spell(self):
        spell = self.edid["HighrimRavage"]
        spit = spell.first("SPIT")
        self.assertEqual(struct.unpack_from("<I", spit, 8)[0], 0, "spell")
        self.assertEqual(struct.unpack_from("<I", spit, 16)[0], 1, "fire and forget")
        self.assertEqual(struct.unpack_from("<I", spit, 20)[0], 0, "self")
        fx = self.effects_of("HighrimRavage")
        self.assertEqual([f for f, _ in fx], [self.edid[e].formid for e in
                         ("HighrimFxRavageHealth", "HighrimFxRavageMagicka", "HighrimFxRavageStamina")])
        for f, (mag, area, dur) in fx:
            self.assertEqual((mag, dur), (0.0, 99999), "magnitude written by the plugin before each cast; long like Survival Mode's")
        for edid, av in (("HighrimFxRavageHealth", 24), ("HighrimFxRavageMagicka", 25), ("HighrimFxRavageStamina", 26)):
            data = self.edid[edid].first("DATA")
            self.assertEqual(struct.unpack_from("<I", data, 64)[0], 34, "peak value modifier: lowers the maximum")
            self.assertEqual(struct.unpack_from("<I", data, 68)[0], av)
            self.assertEqual(struct.unpack_from("<I", data, 0)[0] & 0x5, 0, "not hostile or detrimental")
            self.assertFalse(self.edid[edid].get("KWDA"))

    def test_timed_addicted_and_withdrawal_spells(self):
        by_id = {r.formid: r for r in self.plugin.records}
        for rec in [self.edid[e] for e in timed_edids()]:
            spit = rec.first("SPIT")
            self.assertEqual(struct.unpack_from("<I", spit, 8)[0], 0, f"{rec.edid} is a timed spell, not an ability")
            self.assertEqual(struct.unpack_from("<I", spit, 16)[0], 1, "fire and forget")
            self.assertEqual(struct.unpack_from("<I", spit, 20)[0], 0, "self")
            durations = {d for _, (_, _, d) in self.effects_of(rec.edid)}
            self.assertEqual(len(durations), 1, f"{rec.edid}: one duration for all effects")
            self.assertGreater(durations.pop(), 0)
        # base spells: the placeholder duration; variants: the fixed lengths of Core::kTimedVariantSeconds
        for key, name in (("Rimerush", "Rimerush"), ("VelvetHaze", "Velvet Haze"), ("Dreamcap", "Dreamcap")):
            base = self.edid[f"HighrimTimedAddicted{key}"]
            self.assertEqual(base.first("FULL"), f"Addicted: {name}".encode("ascii") + bytes(1))
            self.assertEqual(base.first("DESC"), f"Dependent on {name} until the timer ends.".encode("ascii") + bytes(1))
            self.assertEqual([f for f, _ in self.effects_of(base.edid)], [self.edid["HighrimFxAddicted"].formid])
            self.assertEqual(self.effects_of(base.edid)[0][1], (0.0, 0, 3600))
            for v, secs in enumerate(TIMED_VARIANT_SECONDS):
                var = self.edid[f"HighrimTimedAddicted{key}V{v}"]
                self.assertEqual(var.first("FULL"), base.first("FULL"))
                self.assertEqual(self.effects_of(var.edid)[0][1], (0.0, 0, secs))
        for tier, penalty in ((1, -10), (2, -20), (3, -30)):
            base = self.edid[f"HighrimTimedWithdrawal{tier}"]
            old = self.edid[f"HighrimWithdrawal{tier}"]
            self.assertEqual(base.first("FULL"), old.first("FULL"))
            self.assertEqual(base.first("DESC"), old.first("DESC"), "same words as the ability")
            fx = self.effects_of(base.edid)
            skills = [e for f, e in fx if by_id[f].edid.startswith("HighrimFxWdSkill")]
            self.assertEqual(len(skills), 18)
            self.assertTrue(all(m == penalty for m, _, _ in skills))
            self.assertEqual(len({f for f, _ in fx}), len(fx))
            self.assertEqual(len(self.visible(base.edid)), len(self.visible(old.edid)), "same visible lines as the ability")
            regen = [m for f, (m, _, _) in fx if f == self.edid["HighrimFxWdRegenDown"].formid]
            self.assertEqual(regen, [{1: -20, 2: -35, 3: -50}[tier]])
            for v in range(len(TIMED_VARIANT_SECONDS)):
                var = self.edid[f"HighrimTimedWithdrawal{tier}V{v}"]
                self.assertEqual([f for f, _ in self.effects_of(var.edid)], [f for f, _ in fx])
                self.assertEqual({d for _, (_, _, d) in self.effects_of(var.edid)}, {TIMED_VARIANT_SECONDS[v]})
        # the old abilities stay (saves made before still point at them)
        for e in ("HighrimWithdrawal1", "HighrimWithdrawal2", "HighrimWithdrawal3", "HighrimAddictedRimerush"):
            self.assertEqual(struct.unpack_from("<I", self.edid[e].first("SPIT"), 8)[0], 4)

    def test_timed_variant_table_matches_core_and_forms_h(self):
        core = read_text(os.path.join(ROOT, "src", "Core.h"))
        match = re.search(r"kTimedVariantSeconds\{([^}]*)\}", core)
        self.assertIsNotNone(match)
        self.assertEqual(tuple(int(float(v)) for v in match.group(1).split(",")), TIMED_VARIANT_SECONDS)
        header = read_text(self.hdr)
        self.assertIn(f"kTimedVariantCount = {len(TIMED_VARIANT_SECONDS)};", header)
        rows = re.search(r"kTimedAddicted\[3\]\[\d+\] = \{(.*?)\};", header, re.S).group(1)
        first = [int(x, 16) for x in re.findall(r"0x([0-9A-F]{6})", rows)]
        self.assertEqual(len(first), 3 * (len(TIMED_VARIANT_SECONDS) + 1))
        self.assertEqual(first[0], self.edid["HighrimTimedAddictedRimerush"].formid & 0xFFFFFF)
        self.assertEqual(first[1], self.edid["HighrimTimedAddictedRimerushV0"].formid & 0xFFFFFF)
        self.assertEqual(first[len(TIMED_VARIANT_SECONDS) + 1], self.edid["HighrimTimedAddictedVelvetHaze"].formid & 0xFFFFFF)
        rows = re.search(r"kTimedWithdrawal\[3\]\[\d+\] = \{(.*?)\};", header, re.S).group(1)
        second = [int(x, 16) for x in re.findall(r"0x([0-9A-F]{6})", rows)]
        self.assertEqual(second[-1], self.edid[f"HighrimTimedWithdrawal3V{len(TIMED_VARIANT_SECONDS) - 1}"].formid & 0xFFFFFF)

    # --- BOOK -------------------------------------------------------------------
    def test_nine_notes_with_exact_text(self):
        books = self.plugin.by_type("BOOK")
        self.assertEqual(len(books), 9)
        for hold in HOLDS:
            book = self.edid["HighrimNote" + hold]
            text = book.first("DESC")
            expected = ("Last night is a blur. Shouting. Guards. A cold floor, maybe a cell. Your things might be somewhere in "
                        f"{CAPITALS[hold]}... if the guards kept them.")
            self.assertEqual(text, expected.encode("ascii") + b"\0")
            self.assertNotIn(b"rison", text)

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
        imods = {k: v.upper().zfill(6) for k, v in found.items() if k.startswith("kImod")}
        self.assertEqual(list(imods), ["kImodHallucinogen"], "only Dreamcap uses a vanilla screen effect (Velvet Haze has its own IMAD, Rimerush none)")
        self.assertNotIn("kImodStimulant", text)
        own = {k: v for k, v in found.items() if not k.startswith("kImod")}
        expected = {r.edid: f"{r.formid & 0xFFFFFF:06X}" for r in self.plugin.records}
        self.assertEqual({k: v.upper() for k, v in own.items()}, expected)
        self.vanilla_imods = imods
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
                    if tag == "CNTO":
                        self.assertEqual(vanilla[formid], "MISC", f"{record.edid} {tag} {formid:08X} (gold)")
                    elif expectations[tag]:
                        self.assertEqual(vanilla[formid], expectations[tag], f"{record.edid} {tag} {formid:08X}")
                    seen += 1
        self.assertGreater(seen, 20)
        self.test_forms_header_matches_the_plugin()
        for name, local in self.vanilla_imods.items():
            self.assertEqual(vanilla.get(int(local, 16)), "IMAD", f"{name} {local} is not an IMAD in Skyrim.esm")


if __name__ == "__main__":
    unittest.main(verbosity=2)
