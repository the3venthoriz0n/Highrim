"""Generates package/Data/Highrim.esp (light/ESL plugin) and src/Forms.h from vanilla templates.

Every record except the MCM quest is a clone of a vanilla Skyrim.esm record (copied into
tools/vanilla_templates.json by tools/extract_templates.py) with fields changed: new EDID, inline name and
text strings (this plugin is not localized), new effects and values. No record layout is hand-invented.

Usage: python -I tools/make_esp.py [<output.esp> [<Forms.h>]]
Defaults: package/Data/Highrim.esp and src/Forms.h next to this repository's tools folder.

Records, by fixed EDID (form IDs are assigned in this order from 0x800; the MCM quest keeps 0xD62):
  KYWD  HighrimDrug                                 keyword on all three drugs (for the later dealer feature)
  MGEF  HighrimFxRimerushSpeed / Stamina / Regen    Rimerush effects (clones of potion effects)
        HighrimFxVelvetResist / VelvetSlow          Velvet Haze effects
        HighrimFxDetectEnemyOut / EnemyIn / FriendOut / FriendIn   Detect Life copies (description on one only)
        HighrimFxRegenDown                          stamina regen drain, shared by comedowns (potion clone)
        HighrimAbRegenDown / AbStaminaDown / AbHealRateDown   constant-effect clones for withdrawal/hangover
  SPEL  HighrimComedownRimerush / VelvetHaze / Dreamcap   60 s fire-and-forget spells the plugin casts at the end of a high
        HighrimWithdrawal1 / 2 / 3                  withdrawal abilities (type Ability), one set for all drug types
        HighrimHangover                             ability the plugin adds after a blackout and removes after a game day
        HighrimBlurPulse                            8 s blur the plugin casts now and then (withdrawal tier 2, hangover)
  BOOK  HighrimNote<Hold>                           nine notes: Whiterun Eastmarch Rift Haafingar Hjaalmarch Reach
                                                    Falkreath Pale Winterhold
  ALCH  HighrimRimerush / HighrimVelvetHaze / HighrimDreamcap
  COBJ  HighrimRecipeRimerush / RecipeVelvetHaze / RecipeDreamcap   cooking pot recipes
  QUST  Highrim_MCM                                 MCM Helper registration quest (unchanged from Task 1)
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esp  # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
PLUGIN_NAME = "Highrim.esp"
MOD_NAME = "Highrim"
AUTHOR = "the3venthoriz0n"
OWN = 0x01000000          # master index 1 = this plugin (master 0 is Skyrim.esm)
FIRST_LOCAL_ID = 0x800    # light plugin range is 0x800..0xFFF
MCM_QUEST_ID = OWN | 0xD62
NEXT_OBJECT_ID = 0xD63
ESL = 0x200

HOLDS = ["Whiterun", "Eastmarch", "Rift", "Haafingar", "Hjaalmarch", "Reach", "Falkreath", "Pale", "Winterhold"]

# Vanilla form IDs used by reference (all confirmed in Skyrim.esm by tests/test_esp.py).
GIANT_LICHEN, GLOWING_MUSHROOM, LAVENDER, HANGING_MOSS, SWAMP_FUNGAL_POD = (
    0x0007E8C1, 0x0007EE01, 0x00045C28, 0x00057F91, 0x0007E8B7)
COOKPOT_KEYWORD = 0x000A5CB3
VENDOR_ITEM_POTION = 0x0008CDEC
MAGIC_ALCH_BENEFICIAL = 0x000F8A4E
DRUGGED_EFFECT = 0x000E3CBB   # dunSleepingTreeDruggedAlch: the sleeping tree sap trip (script driven image space)
# The four Detect Life effects the vanilla Detect Life scroll (TGDetectLifeScroll) uses, with its areas
# and per-effect condition (CTDA, copied byte for byte from that scroll).
CTDA_ENEMY = bytes.fromhex("2000000000000000cf02000000000000000000000000000000000000ffffffff")
CTDA_FRIEND = bytes.fromhex("0000000000000000cf02000000000000000000000000000000000000ffffffff")
# Our own copies of the four vanilla MGEFs (see MGEFS): the vanilla Interior ones carry a description string
# and the Exterior ones none, which made the potion tooltip repeat the text. Each copy keeps the vanilla
# MGEF conditions (interior/exterior, not undead/daedra/construct) untouched.
DETECT_LIFE = [  # (our MGEF EDID, area in feet, condition)
    ("HighrimFxDetectEnemyOut", 200, CTDA_ENEMY),
    ("HighrimFxDetectEnemyIn", 100, CTDA_ENEMY),
    ("HighrimFxDetectFriendOut", 200, CTDA_FRIEND),
    ("HighrimFxDetectFriendIn", 100, CTDA_FRIEND),
]
DETECT_LIFE_TEXT = "Living creatures and people are visible through walls within <area> feet for <dur> seconds."

# Template form IDs (see tools/extract_templates.py).
T_FORT_STAMINA, T_FORT_STAMINA_RATE, T_SAP_SLOW, T_DMG_STAMINA_RATE = "0003EAF9", "0003EB08", "000E3CBA", "00073F2C"
T_AB_DMG_STAMINA_RATE, T_AB_FORT_STAMINA, T_AB_DMG_HEAL_RATE = "000C1E8D", "00091F77", "0010DE1A"
T_ALCH, T_BOOK, T_ABILITY, T_SPELL, T_COBJ, T_KYWD = "00057A7A", "000E1A9F", "0010FB30", "0005AD5C", "000F431F", "0008CDEC"

ACTOR_VALUE_SPEED_MULT = 30
ACTOR_VALUE_DAMAGE_RESIST = 39

# (EDID, name, description, template, overrides)
MGEFS = [
    ("HighrimFxRimerushSpeed", "Rush Speed", "Movement speed is increased by <mag>% for <dur> seconds.",
     T_FORT_STAMINA, {"av": ACTOR_VALUE_SPEED_MULT, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimFxRimerushStamina", "Rush Stamina", "Stamina is increased by <mag> points for <dur> seconds.",
     T_FORT_STAMINA, {}),
    ("HighrimFxRimerushRegen", "Rush Recovery", "Stamina regenerates <mag>% faster for <dur> seconds.",
     T_FORT_STAMINA_RATE, {}),
    ("HighrimFxVelvetResist", "Velvet Armor", "Armor rating is increased by <mag> for <dur> seconds.",
     T_FORT_STAMINA, {"av": ACTOR_VALUE_DAMAGE_RESIST, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimFxVelvetSlow", "Velvet Drag", "Movement is slowed by <mag>% for <dur> seconds.", T_SAP_SLOW, {}),
    ("HighrimFxRegenDown", "Slowed Recovery", "Stamina regenerates <mag>% slower for <dur> seconds.",
     T_DMG_STAMINA_RATE, {"drop": ["VMAD"]}),
    ("HighrimFxDetectEnemyOut", "Detect Life", DETECT_LIFE_TEXT, "001019D8", {}),
    ("HighrimFxDetectEnemyIn", "Detect Life", "", "001019D9", {"drop": ["DNAM"]}),
    ("HighrimFxDetectFriendOut", "Detect Life", "", "001019DA", {"drop": ["DNAM"]}),
    ("HighrimFxDetectFriendIn", "Detect Life", "", "001019DB", {"drop": ["DNAM"]}),
    ("HighrimAbRegenDown", "Cravings", "Stamina regenerates <mag>% slower.", T_AB_DMG_STAMINA_RATE, {}),
    ("HighrimAbStaminaDown", "Shakes", "Stamina is changed by <mag> points.", T_AB_FORT_STAMINA, {}),
    ("HighrimAbHealRateDown", "Slow Healing", "Health regenerates <mag>% slower.", T_AB_DMG_HEAL_RATE, {}),
]


def fx(edid):
    return ids[edid]


def effect(mgef, magnitude, duration, area=0, conditions=()):
    return {"mgef": mgef, "mag": magnitude, "area": area, "dur": duration, "ctda": list(conditions)}


def spell_defs():
    """(EDID, name, kind, effects) with kind 'ability' or 'spell'."""
    return [
        ("HighrimComedownRimerush", "Rimerush Comedown", "spell", [effect(fx("HighrimFxRegenDown"), 60, 60)]),
        ("HighrimComedownVelvetHaze", "Velvet Haze Comedown", "spell", [effect(fx("HighrimFxRegenDown"), 40, 60)]),
        ("HighrimComedownDreamcap", "Dreamcap Comedown", "spell", [effect(DRUGGED_EFFECT, 0, 60)]),
        ("HighrimWithdrawal1", "Withdrawal: Cravings", "ability",
         [effect(fx("HighrimAbRegenDown"), 20, 0)]),
        ("HighrimWithdrawal2", "Withdrawal: Shakes", "ability",
         [effect(fx("HighrimAbRegenDown"), 35, 0), effect(fx("HighrimAbStaminaDown"), -25, 0)]),
        ("HighrimWithdrawal3", "Withdrawal: Sickness", "ability",
         [effect(fx("HighrimAbRegenDown"), 50, 0), effect(fx("HighrimAbStaminaDown"), -40, 0),
          effect(fx("HighrimAbHealRateDown"), 50, 0)]),
        ("HighrimHangover", "Hangover", "ability",
         [effect(fx("HighrimAbStaminaDown"), -30, 0), effect(fx("HighrimAbRegenDown"), 25, 0)]),
        ("HighrimBlurPulse", "Blurred Vision", "spell", [effect(DRUGGED_EFFECT, 0, 8)]),
    ]


def drug_defs():
    """(EDID, name, value, effects)."""
    high = 120
    return [
        ("HighrimRimerush", "Rimerush", 60,
         [effect(fx("HighrimFxRimerushSpeed"), 25, high), effect(fx("HighrimFxRimerushStamina"), 50, high),
          effect(fx("HighrimFxRimerushRegen"), 100, high)]),
        ("HighrimVelvetHaze", "Velvet Haze", 55,
         [effect(fx("HighrimFxVelvetResist"), 150, high), effect(fx("HighrimFxVelvetSlow"), 20, high)]),
        ("HighrimDreamcap", "Dreamcap", 70,
         [effect(DRUGGED_EFFECT, 0, high)]
         + [effect(fx(mgef), 0, high, area, [ctda]) for mgef, area, ctda in DETECT_LIFE]),
    ]


RECIPES = [  # (EDID, product EDID, [(ingredient, count)])
    ("HighrimRecipeRimerush", "HighrimRimerush", [(GIANT_LICHEN, 2), (GLOWING_MUSHROOM, 1)]),
    ("HighrimRecipeVelvetHaze", "HighrimVelvetHaze", [(LAVENDER, 2), (HANGING_MOSS, 1)]),
    ("HighrimRecipeDreamcap", "HighrimDreamcap", [(GLOWING_MUSHROOM, 2), (SWAMP_FUNGAL_POD, 1)]),
]

KEYWORD_EDID = "HighrimDrug"
SPELL_EDIDS = ["HighrimComedownRimerush", "HighrimComedownVelvetHaze", "HighrimComedownDreamcap",
               "HighrimWithdrawal1", "HighrimWithdrawal2", "HighrimWithdrawal3", "HighrimHangover",
               "HighrimBlurPulse"]
NOTE_EDIDS = ["HighrimNote" + hold for hold in HOLDS]
ALCH_EDIDS = ["HighrimRimerush", "HighrimVelvetHaze", "HighrimDreamcap"]
COBJ_EDIDS = [r[0] for r in RECIPES]
# Group order and the order form IDs are handed out in.
ORDER = [("KYWD", [KEYWORD_EDID]), ("MGEF", [m[0] for m in MGEFS]), ("SPEL", SPELL_EDIDS),
         ("BOOK", NOTE_EDIDS), ("ALCH", ALCH_EDIDS), ("COBJ", COBJ_EDIDS)]

ids = {}
for _rtype, _names in ORDER:
    for _name in _names:
        ids[_name] = OWN | (FIRST_LOCAL_ID + len(ids))
assert len(ids) <= 0xFFF - FIRST_LOCAL_ID + 1 and max(ids.values()) < MCM_QUEST_ID


# --- template helpers --------------------------------------------------------------------------------
def load_templates():
    with open(os.path.join(TOOLS, "vanilla_templates.json"), encoding="utf-8") as handle:
        raw = json.load(handle)
    return {k: (v["type"], [(tag, bytes.fromhex(data)) for tag, data in v["subs"]]) for k, v in raw.items()}


TEMPLATES = load_templates()


def template(key, expect_type):
    rtype, subs = TEMPLATES[key]
    assert rtype == expect_type, (key, rtype)
    return [(tag, data) for tag, data in subs]


def replace(subs, tag, data):
    for index, (existing, _) in enumerate(subs):
        if existing == tag:
            subs[index] = (tag, data)
            return
    raise KeyError(tag)


def drop(subs, *tags):
    subs[:] = [(t, d) for t, d in subs if t not in tags]


def set_keywords(subs, keywords):
    drop(subs, "KSIZ", "KWDA")
    at = next(i for i, (t, _) in enumerate(subs) if t == "DATA")
    subs[at:at] = [("KSIZ", struct.pack("<I", len(keywords))),
                   ("KWDA", b"".join(struct.pack("<I", k) for k in keywords))]


def patch(data, offset, fmt, value):
    size = struct.calcsize(fmt)
    return data[:offset] + struct.pack(fmt, value) + data[offset + size:]


MGEF_ASSOC_OFFSET = 8     # associated item (keyword) in MGEF DATA
MGEF_AV_OFFSET = 68       # actor value in MGEF DATA (follows archetype at 64)


def build_mgef(edid, name, description, key, overrides):
    subs = template(key, "MGEF")
    replace(subs, "EDID", esp.zstr(edid))
    replace(subs, "FULL", esp.zstr(name))
    if "DNAM" not in overrides.get("drop", []):
        replace(subs, "DNAM", esp.zstr(description))
    drop(subs, *overrides.get("drop", []))
    data = dict(subs)["DATA"]
    if "assoc" in overrides:
        data = patch(data, MGEF_ASSOC_OFFSET, "<I", overrides["assoc"])
    if "av" in overrides:
        data = patch(data, MGEF_AV_OFFSET, "<I", overrides["av"])
    replace(subs, "DATA", data)
    if "keywords" in overrides:
        set_keywords(subs, overrides["keywords"])
    return esp.record(b"MGEF", ids[edid], esp.join_subs(subs))


def effect_subs(effects):
    out = []
    for item in effects:
        out.append(("EFID", struct.pack("<I", item["mgef"])))
        out.append(("EFIT", struct.pack("<fII", float(item["mag"]), item["area"], item["dur"])))
        out.extend(("CTDA", ctda) for ctda in item["ctda"])
    return out


def build_spell(edid, name, kind, effects):
    subs = template(T_ABILITY, "SPEL")
    replace(subs, "EDID", esp.zstr(edid))
    replace(subs, "FULL", esp.zstr(name))
    replace(subs, "DESC", esp.zstr(name))
    if kind == "ability":
        spit = struct.pack("<IIIfIIffI", 0, 0, 4, 0.0, 0, 0, 0.0, 0.0, 0)
    else:
        # Oakflesh's layout: type Spell, 0.5 s charge, fire and forget, self; cost fixed at 0 (manual calc).
        spit = struct.pack("<IIIfIIffI", 0, 1, 0, 0.5, 1, 0, 0.0, 0.0, 0)
    replace(subs, "SPIT", spit)
    drop(subs, "EFID", "EFIT")
    subs.extend(effect_subs(effects))
    return esp.record(b"SPEL", ids[edid], esp.join_subs(subs))


def build_book(edid, hold):
    subs = template(T_BOOK, "BOOK")
    replace(subs, "EDID", esp.zstr(edid))
    replace(subs, "FULL", esp.zstr(f"Crumpled Note ({hold})"))
    replace(subs, "DESC", esp.zstr(f"Your things are in the {hold} prison."))
    return esp.record(b"BOOK", ids[edid], esp.join_subs(subs))


def build_alch(edid, name, value, effects):
    subs = template(T_ALCH, "ALCH")
    replace(subs, "EDID", esp.zstr(edid))
    replace(subs, "FULL", esp.zstr(name))
    drop(subs, "KSIZ", "KWDA")
    at = next(i for i, (t, _) in enumerate(subs) if t == "MODL")
    subs[at:at] = [("KSIZ", struct.pack("<I", 2)),
                   ("KWDA", struct.pack("<II", VENDOR_ITEM_POTION, ids[KEYWORD_EDID]))]
    enit = dict(subs)["ENIT"]
    sound = enit[16:20]
    # value, flags (1 = no auto-calculation, so the value is the one written), addiction 0, chance 0, use sound.
    replace(subs, "ENIT", struct.pack("<IIII", value, 1, 0, 0) + sound)
    drop(subs, "EFID", "EFIT")
    subs.extend(effect_subs(effects))
    return esp.record(b"ALCH", ids[edid], esp.join_subs(subs))


def build_cobj(edid, product, ingredients):
    subs = template(T_COBJ, "COBJ")
    replace(subs, "EDID", esp.zstr(edid))
    drop(subs, "COCT", "CNTO", "CNAM")
    new = [("COCT", struct.pack("<I", len(ingredients)))]
    new += [("CNTO", struct.pack("<II", item, count)) for item, count in ingredients]
    new.append(("CNAM", struct.pack("<I", ids[product])))
    at = next(i for i, (t, _) in enumerate(subs) if t == "BNAM")
    subs[at:at] = new
    replace(subs, "BNAM", struct.pack("<I", COOKPOT_KEYWORD))
    replace(subs, "NAM1", struct.pack("<H", 1))
    return esp.record(b"COBJ", ids[edid], esp.join_subs(subs))


def build_keyword(edid):
    subs = template(T_KYWD, "KYWD")
    replace(subs, "EDID", esp.zstr(edid))
    return esp.record(b"KYWD", ids[edid], esp.join_subs(subs))


def build_mcm_quest(mod_name):
    """The MCM Helper registration quest, byte for byte what tools/make_mcm_plugin.py wrote in Task 1."""
    quest_id = MCM_QUEST_ID  # master index 1 (this plugin), light-plugin local id 0xD62
    editor_id = f"{mod_name}_MCM"
    # Papyrus data: one script (MCM_ConfigBase, no property overrides), then the quest's alias block
    # with SKI_PlayerLoadGameAlias on the player alias. The alias block is copied byte for byte from a
    # working MCM Helper quest (Better Third Person Selection); it holds this quest's own FormID.
    vmad = struct.pack("<HHH", 5, 2, 1)
    vmad += esp.wstring("MCM_ConfigBase") + struct.pack("<BH", 0, 0)
    vmad += bytes.fromhex("0200000000010000000000") + struct.pack("<I", quest_id)
    vmad += bytes.fromhex("0500020001001700") + b"SKI_PlayerLoadGameAlias" + bytes.fromhex("000000")
    quest = [
        ("EDID", esp.zstr(editor_id)),
        ("VMAD", vmad),
        ("FULL", esp.zstr(editor_id)),
        ("DNAM", bytes.fromhex("010100b000000000" "00000000")),  # start game enabled
        ("NEXT", b""),
        ("ANAM", struct.pack("<I", 1)),
        ("ALST", struct.pack("<I", 0)),
        ("ALID", esp.zstr("PlayerAlias")),
        ("FNAM", struct.pack("<I", 0)),
        ("ALFR", struct.pack("<I", 0x14)),
        ("VTCK", struct.pack("<I", 0)),
        ("ALED", b""),
    ]
    return esp.record(b"QUST", quest_id, esp.join_subs(quest))


def build():
    """Returns (esp bytes, [(EDID, local form id)] sorted by id)."""
    spells = {d[0]: d for d in spell_defs()}
    drugs = {d[0]: d for d in drug_defs()}
    records = {
        "KYWD": [build_keyword(KEYWORD_EDID)],
        "MGEF": [build_mgef(*m) for m in MGEFS],
        "SPEL": [build_spell(*spells[e]) for e in SPELL_EDIDS],
        "BOOK": [build_book("HighrimNote" + hold, hold) for hold in HOLDS],
        "ALCH": [build_alch(*drugs[e]) for e in ALCH_EDIDS],
        "COBJ": [build_cobj(*r) for r in RECIPES],
        "QUST": [build_mcm_quest(MOD_NAME)],
    }
    groups = [esp.group(rtype.encode("ascii"), records[rtype]) for rtype in
              [o[0] for o in ORDER] + ["QUST"]]
    count = sum(len(v) for v in records.values()) + len(groups)
    header = esp.tes4(["Skyrim.esm"], AUTHOR, count, NEXT_OBJECT_ID, ESL)
    forms = {name: formid & 0xFFFFFF for name, formid in ids.items()}
    forms[f"{MOD_NAME}_MCM"] = MCM_QUEST_ID & 0xFFFFFF
    return header + b"".join(groups), sorted(forms.items(), key=lambda kv: kv[1])


def forms_header(forms):
    lines = [
        "// Generated by tools/make_esp.py from the plugin it writes. Do not edit.",
        "// Local form IDs (the part below the load-order byte) by EDID; combine with the plugin's load order",
        "// index at runtime (for a light plugin use the light index) or look the form up by plugin name.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace Highrim::Forms",
        "{",
        f'    inline constexpr const char* kPluginName = "{PLUGIN_NAME}";',
        "",
    ]
    for name, local in forms:
        lines.append(f"    inline constexpr std::uint32_t {name} = 0x{local:06X};")
    lines += ["}", ""]
    return "\n".join(lines)


def main(argv):
    out_esp = argv[1] if len(argv) > 1 else os.path.join(ROOT, "package", "Data", PLUGIN_NAME)
    out_header = argv[2] if len(argv) > 2 else os.path.join(ROOT, "src", "Forms.h")
    data, forms = build()
    with open(out_esp, "wb") as handle:
        handle.write(data)
    with open(out_header, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(forms_header(forms))
    print(f"Wrote {out_esp} ({len(data)} bytes) and {out_header} ({len(forms)} form IDs)")


if __name__ == "__main__":
    main(sys.argv)
