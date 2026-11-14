"""Generates package/Data/Highrim.esp (light/ESL plugin) and src/Forms.h from vanilla templates.

Every record except the MCM quest is a clone of a vanilla Skyrim.esm record (copied into
tools/vanilla_templates.json by tools/extract_templates.py) with fields changed: new EDID, inline name and
text strings (this plugin is not localized), new effects and values. No record layout is hand-invented.

Usage: python -I tools/make_esp.py [<output.esp> [<Forms.h>]]
Defaults: package/Data/Highrim.esp and src/Forms.h next to this repository's tools folder.

Records, by fixed EDID (form IDs: the 0.1.0 records are pinned in tools/pinned_form_ids.json;
new ones are allocated in this order after the highest pinned ID, see allocate_ids):
  KYWD  HighrimDrug                                 keyword on all three drugs (for the later dealer feature)
  MGEF  HighrimFxRimerushSpeed / Stamina / Regen    Rimerush effects (clones of potion effects)
        HighrimFxVelvetResist / VelvetSlow          Velvet Haze effects
        HighrimFxDetectLife                         one Detect Life effect with no conditions (no longer on Dreamcap: the plugin casts vanilla Detect Life itself; kept because its ID is in use)
        HighrimFxRegenDown                          stamina regen change, shared by comedowns (non-detrimental clone, negative magnitude)
        HighrimAbRegenDown / AbStaminaDown / AbHealRateDown   constant-effect clones for withdrawal/hangover
        HighrimAbSpeedUp / AbStaminaUp / AbRegenUp / AbResistUp / AbSlow   constant-effect clones for the dose stacks
  SPEL  HighrimComedownRimerush / VelvetHaze / Dreamcap   60 s fire-and-forget spells the plugin casts at the end of a high
        HighrimWithdrawal1 / 2 / 3                  withdrawal abilities (type Ability), one set for all drug types
        HighrimHangover                             ability the plugin adds after a blackout and removes after a game day
        HighrimStack<Rimerush|VelvetHaze|Dreamcap>1..3   dose stack abilities (type Ability), one at a time per drug
        HighrimBlurPulse                            8 s blur the plugin casts now and then (withdrawal tier 2, hangover)
        HighrimLostGear<Hold>                       nine abilities (one per hold) that say where the player's things are
        HighrimAbLostGear                           the (harmless) effect those abilities carry
  IMAD  HighrimImodVelvet                           Velvet Haze screen effect: ParalyzeMassImod with the blur tracks zeroed
        HighrimImodComedown                         black-and-white comedown look: the same record, no blur, no tint, saturation 0, 60 s
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
ESL = 0x200

HOLDS = ["Whiterun", "Eastmarch", "Rift", "Haafingar", "Hjaalmarch", "Reach", "Falkreath", "Pale", "Winterhold"]

# Vanilla form IDs used by reference (all confirmed in Skyrim.esm by tests/test_esp.py).
GIANT_LICHEN, GLOWING_MUSHROOM, LAVENDER, HANGING_MOSS, SWAMP_FUNGAL_POD = (
    0x0007E8C1, 0x0007EE01, 0x00045C28, 0x00057F91, 0x0007E8B7)
COOKPOT_KEYWORD = 0x000A5CB3
VENDOR_ITEM_POTION = 0x0008CDEC
MAGIC_ALCH_BENEFICIAL = 0x000F8A4E
# Screen effects the plugin triggers itself (vanilla IMAD records in Skyrim.esm), one per drug type. Picked by
# name only; what they look like in game is unverified. Candidates considered are listed in the README.
IMOD_STIMULANT = 0x0010C761     # QuickReflexesImod: adrenaline rush (bright, contrasty)
IMOD_DEPRESSANT_SOURCE = 0x00031060  # ParalyzeMassImod: Velvet Haze's own IMAD (HighrimImodVelvet) is cloned from it
IMOD_HALLUCINOGEN = 0x000C1966  # DA13PeryiteVisionImod: strong tinted vision
IMODS = [("kImodStimulant", IMOD_STIMULANT), ("kImodHallucinogen", IMOD_HALLUCINOGEN)]
IMAD_VELVET = "HighrimImodVelvet"
# IMAD subrecords that hold blur: blur radius, double vision, radial blur strength / ramp up / ramp down / start /
# rotation, depth of field strength / distance / range, motion blur. Each is a list of (time, value) float pairs; the
# values are zeroed and the times and key counts (DNAM) are left alone. Colour tracks (tint, saturation, contrast) stay.
IMAD_BLUR_TAGS = ["BNAM", "VNAM", "RNAM", "SNAM", "UNAM", "NAM1", "NAM2", "WNAM", "XNAM", "YNAM", "NAM4"]
DRUGGED_EFFECT = 0x000E3CBB   # dunSleepingTreeDruggedAlch: the sleeping tree sap trip (script driven image space)
# One Detect Life effect of our own, a copy of the vanilla exterior one (DetectLifeEnemyExteriorSelfFF) with every
# condition removed. The vanilla scroll uses four effects: per-effect conditions IsHostileToActor (enemy / friend split,
# which depends on the player's relations) and IsInInterior (area 100 / 200 ft), plus target filters (undead, daedra,
# dwarven, NoDetectLife, IsUndead). A condition that fails for one player but not another means no detection for that
# player, so none are kept: one effect, one area, same for everybody. Undead, daedra and constructs now show too.
DETECT_LIFE_AREA = 150
DETECT_LIFE_TEXT = ("Living creatures and people are visible through walls within <area> feet for <dur> seconds. "
                    "(Not on the Dreamcap potion any more: the plugin casts the vanilla Detect Life spell instead.)")
GOLD = 0x0000000F             # Gold001
RECIPE_GOLD = 1000            # every cooking pot recipe costs exactly this much gold and nothing else
IMAD_COMEDOWN = "HighrimImodComedown"
COMEDOWN_IMOD_SECONDS = 60.0  # duration written into the comedown IMAD (the plugin also stops it by itself)
ACTOR_VALUE_CARRY_WEIGHT = 32

# Template form IDs (see tools/extract_templates.py).
T_FORT_STAMINA, T_FORT_STAMINA_RATE, T_SAP_SLOW = "0003EAF9", "0003EB08", "000E3CBA"
T_AB_FORT_STAMINA = "00091F77"
T_ALCH, T_BOOK, T_ABILITY, T_SPELL, T_COBJ, T_KYWD = "00057A7A", "000E1A9F", "0010FB30", "0005AD5C", "000F431F", "0008CDEC"

ACTOR_VALUE_SPEED_MULT = 30
ACTOR_VALUE_DAMAGE_RESIST = 39
ACTOR_VALUE_STAMINA_RATE = 0x9D   # the value AbDamageStaminaRate and AlchFortifyStaminaRate change
ACTOR_VALUE_HEAL_RATE = 0x9B      # the value VampireAbDamageHealRate changes

# Dose stacks: stack abilities come in levels 1..3 (stack 2, 3, 4 and beyond), on top of the potion's own effects.
STACK_LEVELS = 3
HIGH_SECONDS = 120   # how long a potion's own effects last

# Potion base magnitudes. Task 9 raised them about 35% (was: speed 25, stamina 50, regen 100; resist 150, slow 20).
RIMERUSH_BASE = {"speed": 34, "stamina": 68, "regen": 135}
VELVET_BASE = {"resist": 200, "slow": 27}

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
    # Comedown, withdrawal and hangover effects are NOT clones of the detrimental (Damage ...) effects any more:
    # those carry the Hostile and Detrimental flags. They are Fortify-style clones with a negative magnitude, no
    # hostile/detrimental flag and no harmful keyword (see the README on Cure Disease).
    ("HighrimFxRegenDown", "Slowed Recovery", "Stamina regeneration is changed by <mag>% for <dur> seconds.",
     T_FORT_STAMINA_RATE, {"drop": ["KSIZ", "KWDA"]}),
    ("HighrimFxDetectLife", "Detect Life", DETECT_LIFE_TEXT, "001019D8", {"drop": ["CTDA"]}),
    ("HighrimAbRegenDown", "Cravings", "Stamina regeneration is changed by <mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_STAMINA_RATE}),
    ("HighrimAbStaminaDown", "Shakes", "Stamina is changed by <mag> points.", T_AB_FORT_STAMINA, {}),
    ("HighrimAbHealRateDown", "Slow Healing", "Health regeneration is changed by <mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_HEAL_RATE}),
    # Dose stack effects: constant-effect (ability) clones of AbFortifyStamina with the actor value changed.
    ("HighrimAbSpeedUp", "Surge Speed", "Movement speed is increased by <mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_SPEED_MULT}),
    ("HighrimAbStaminaUp", "Surge Stamina", "Stamina is increased by <mag> points.", T_AB_FORT_STAMINA, {}),
    ("HighrimAbRegenUp", "Surge Recovery", "Stamina regenerates <mag>% faster.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_STAMINA_RATE}),
    ("HighrimAbResistUp", "Heavy Armor", "Armor rating is increased by <mag>.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_DAMAGE_RESIST}),
    ("HighrimAbSlow", "Dragging Limbs", "Movement speed is changed by <mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_SPEED_MULT}),
    # The effect the Lost Gear abilities carry. An ability needs at least one effect to appear in Active Effects, so
    # it is +1 carry weight (harmless); the spell's own description is what tells the player where the gear is.
    ("HighrimAbLostGear", "Lost Gear", "Carry weight is increased by <mag>.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_CARRY_WEIGHT}),
]

# Plain-words text for the spell descriptions of effects whose magnitude is negative (<neg> is the absolute value).
# The effect's own text (above) is what the engine shows for the effect; this is what the spell's tooltip shows.
PLAIN = {
    "HighrimFxRegenDown": "Stamina regenerates <neg>% slower for <dur> seconds.",
    "HighrimAbRegenDown": "Stamina regenerates <neg>% slower.",
    "HighrimAbStaminaDown": "Maximum stamina is <neg> points lower.",
    "HighrimAbHealRateDown": "Health regenerates <neg>% slower.",
    "HighrimAbSlow": "Movement is <neg>% slower.",
}


def fx(edid):
    return ids[edid]


def effect(mgef, magnitude, duration, area=0, conditions=()):
    return {"mgef": mgef, "mag": magnitude, "area": area, "dur": duration, "ctda": list(conditions)}


# Per drug: (EDID prefix, display name, [(MGEF EDID, magnitudes for stack level 1, 2, 3)]). Each effect appears once
# per ability and each level is strictly stronger than the one before. Dreamcap has no stat effect of its own, so
# its stack is a clumsy trip: slowed movement and slowed stamina recovery (a design call, see the README).
STACKS = [
    ("HighrimStackRimerush", "Rimerush Surge",
     [("HighrimAbSpeedUp", (10, 20, 30)), ("HighrimAbStaminaUp", (25, 50, 75)), ("HighrimAbRegenUp", (50, 100, 150))]),
    ("HighrimStackVelvetHaze", "Velvet Haze Weight",
     [("HighrimAbResistUp", (50, 100, 150)), ("HighrimAbSlow", (-8, -16, -24))]),
    ("HighrimStackDreamcap", "Dreamcap Daze",
     [("HighrimAbSlow", (-5, -10, -15)), ("HighrimAbRegenDown", (-15, -30, -45))]),
]
ROMAN = ["I", "II", "III"]
STACK_EDIDS = [f"{prefix}{n}" for prefix, _, _ in STACKS for n in range(1, STACK_LEVELS + 1)]


def stack_spell_defs():
    out = []
    for prefix, name, fxs in STACKS:
        for n in range(1, STACK_LEVELS + 1):
            out.append((f"{prefix}{n}", f"{name} {ROMAN[n - 1]}", "ability",
                        [effect(fx(mgef), mags[n - 1], 0) for mgef, mags in fxs]))
    return out


def spell_defs():
    """(EDID, name, kind, effects) with kind 'ability' or 'spell'."""
    return [
        ("HighrimComedownRimerush", "Rimerush Crash", "spell", [effect(fx("HighrimFxRegenDown"), -60, 60)]),
        ("HighrimComedownVelvetHaze", "Velvet Haze Crash", "spell", [effect(fx("HighrimFxRegenDown"), -40, 60)]),
        ("HighrimComedownDreamcap", "Dreamcap Crash", "spell", [effect(DRUGGED_EFFECT, 0, 60)]),
        ("HighrimWithdrawal1", "Withdrawal I: Cravings", "ability",
         [effect(fx("HighrimAbRegenDown"), -20, 0)]),
        ("HighrimWithdrawal2", "Withdrawal II: Shakes", "ability",
         [effect(fx("HighrimAbRegenDown"), -35, 0), effect(fx("HighrimAbStaminaDown"), -25, 0)]),
        ("HighrimWithdrawal3", "Withdrawal III: Sickness", "ability",
         [effect(fx("HighrimAbRegenDown"), -50, 0), effect(fx("HighrimAbStaminaDown"), -40, 0),
          effect(fx("HighrimAbHealRateDown"), -50, 0)]),
        ("HighrimHangover", "Hangover", "ability",
         [effect(fx("HighrimAbStaminaDown"), -30, 0), effect(fx("HighrimAbRegenDown"), -25, 0)]),
        ("HighrimBlurPulse", "Blurred Vision", "spell", [effect(DRUGGED_EFFECT, 0, 8)]),
    ] + stack_spell_defs() + [
        (f"HighrimLostGear{hold}", f"Lost Gear: {hold} Prison", "ability", [effect(fx("HighrimAbLostGear"), 1, 0)])
        for hold in HOLDS]


# Spell descriptions that say more than the effect texts do (everything else is built from the effects' texts).
SPELL_EXTRA = {
    "HighrimWithdrawal2": " Your vision blurs now and then.",
    "HighrimWithdrawal3": " Your vision blurs now and then.",
    "HighrimHangover": " Your vision blurs now and then. Lasts one game day.",
}
SPELL_TEXT = {f"HighrimLostGear{hold}": f"Your things are in the {hold} prison." for hold in HOLDS}


def drug_defs():
    """(EDID, name, value, effects)."""
    high = HIGH_SECONDS
    return [
        ("HighrimRimerush", "Rimerush", 60,
         [effect(fx("HighrimFxRimerushSpeed"), RIMERUSH_BASE["speed"], high),
          effect(fx("HighrimFxRimerushStamina"), RIMERUSH_BASE["stamina"], high),
          effect(fx("HighrimFxRimerushRegen"), RIMERUSH_BASE["regen"], high)]),
        ("HighrimVelvetHaze", "Velvet Haze", 55,
         [effect(fx("HighrimFxVelvetResist"), VELVET_BASE["resist"], high),
          effect(fx("HighrimFxVelvetSlow"), VELVET_BASE["slow"], high)]),
        ("HighrimDreamcap", "Dreamcap", 70,
         [effect(DRUGGED_EFFECT, 0, high)]),  # reveal life is cast by the plugin (vanilla Detect Life), not a potion effect
    ]


RECIPES = [  # (EDID, product EDID, [(ingredient, count)]): 1000 gold, nothing else (the 0.1.0 plant recipes are gone)
    ("HighrimRecipeRimerush", "HighrimRimerush", [(GOLD, RECIPE_GOLD)]),
    ("HighrimRecipeVelvetHaze", "HighrimVelvetHaze", [(GOLD, RECIPE_GOLD)]),
    ("HighrimRecipeDreamcap", "HighrimDreamcap", [(GOLD, RECIPE_GOLD)]),
]

KEYWORD_EDID = "HighrimDrug"
SPELL_EDIDS = ["HighrimComedownRimerush", "HighrimComedownVelvetHaze", "HighrimComedownDreamcap",
               "HighrimWithdrawal1", "HighrimWithdrawal2", "HighrimWithdrawal3", "HighrimHangover",
               "HighrimBlurPulse"] + STACK_EDIDS + [f"HighrimLostGear{hold}" for hold in HOLDS]
NOTE_EDIDS = ["HighrimNote" + hold for hold in HOLDS]
ALCH_EDIDS = ["HighrimRimerush", "HighrimVelvetHaze", "HighrimDreamcap"]
COBJ_EDIDS = [r[0] for r in RECIPES]
# Group order and the order form IDs are handed out in.
ORDER = [("KYWD", [KEYWORD_EDID]), ("MGEF", [m[0] for m in MGEFS]), ("SPEL", SPELL_EDIDS),
         ("BOOK", NOTE_EDIDS), ("ALCH", ALCH_EDIDS), ("COBJ", COBJ_EDIDS), ("IMAD", [IMAD_VELVET, IMAD_COMEDOWN])]

PINNED_FILE = os.environ.get("HIGHRIM_PINNED_IDS") or os.path.join(TOOLS, "pinned_form_ids.json")


def allocate_ids():
    """EDID -> full form ID. Form IDs are part of the saved-game contract, so they never move.

    Rule: every record that shipped in 0.1.0 keeps the local ID listed in tools/pinned_form_ids.json (the MCM quest
    included). Pins are never edited or reused; a record dropped from the generator is listed under "retired" and
    its ID stays reserved. Every other record gets the next free ID after the highest pinned or retired ID, handed
    out in ORDER (group order, then list order), so the output is reproducible and adding a record at the end of
    its list never moves anything. New records in the middle of a list shift only the unpinned ones after them.
    """
    with open(PINNED_FILE, encoding="utf-8") as handle:
        data = json.load(handle)
    pins = {name: int(value, 16) for name, value in data["pinned"].items()}
    # "provisional": IDs handed out after 0.1.0 by earlier development builds of this branch. They are kept as they
    # are so saves made with those builds still point at the right records; they are not part of the 0.1.0 contract.
    pins.update({name: int(value, 16) for name, value in data.get("provisional", {}).items()})
    retired = set(data.get("retired", []))
    generated = {name for _, names in ORDER for name in names} | {f"{MOD_NAME}_MCM"}
    missing = sorted(set(pins) - generated - retired)
    if missing:
        raise SystemExit(f"pinned records missing from the generator (add them back or list them as retired): {missing}")
    if len(set(pins.values())) != len(pins):
        raise SystemExit("pinned_form_ids.json assigns one ID twice")
    assert pins[f"{MOD_NAME}_MCM"] == MCM_QUEST_ID & 0xFFFFFF
    out = {}
    next_local = max(pins.values()) + 1
    for _rtype, names in ORDER:
        for name in names:
            if name in pins:
                out[name] = OWN | pins[name]
            else:
                out[name] = OWN | next_local
                next_local += 1
    assert min(v & 0xFFFFFF for v in out.values()) >= FIRST_LOCAL_ID and next_local - 1 <= 0xFFF, "outside light range"
    return out, next_local


ids, NEXT_OBJECT_ID = allocate_ids()
assert len(set(ids.values())) == len(ids) and MCM_QUEST_ID not in ids.values()


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


# Text for vanilla effects we reuse that have no description of their own.
VANILLA_EFFECT_TEXT = {DRUGGED_EFFECT: "Your vision swims and blurs for <dur> seconds."}


def spell_description(effects):
    """What the spell does, in plain words, from the descriptions of its effects with the numbers filled in.
    (The spell's own DESC is what the magic menu and tooltips show, so it must say more than the name.)"""
    texts = {}
    for name, _, description, _, _ in MGEFS:
        texts[ids[name]] = PLAIN.get(name, description)
    texts.update(VANILLA_EFFECT_TEXT)
    parts = []
    for item in effects:
        text = texts.get(item["mgef"], "")
        text = (text.replace("<neg>", f"{abs(item['mag']):g}").replace("<mag>", f"{item['mag']:g}")
                .replace("<dur>", str(item["dur"])).replace("<area>", str(item["area"])))
        text = text.replace(" for 0 seconds", "")
        if text and text not in parts:
            parts.append(text)
    return " ".join(parts)


def build_spell(edid, name, kind, effects):
    subs = template(T_ABILITY, "SPEL")
    replace(subs, "EDID", esp.zstr(edid))
    replace(subs, "FULL", esp.zstr(name))
    text = SPELL_TEXT.get(edid) or (spell_description(effects) + SPELL_EXTRA.get(edid, "")) or name
    replace(subs, "DESC", esp.zstr(text))
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


def zero_blur(subs):
    for tag in IMAD_BLUR_TAGS:
        data = dict(subs)[tag]
        assert len(data) % 8 == 0, tag
        floats = list(struct.unpack(f"<{len(data) // 4}f", data))
        floats[1::2] = [0.0] * len(floats[1::2])
        replace(subs, tag, struct.pack(f"<{len(floats)}f", *floats))


def build_imad(edid):
    """Velvet Haze screen effect: ParalyzeMassImod (saturation, contrast, tint kept) without any blur."""
    subs = template(format(IMOD_DEPRESSANT_SOURCE, "08X"), "IMAD")
    replace(subs, "EDID", esp.zstr(edid))
    zero_blur(subs)
    return esp.record(b"IMAD", ids[edid], esp.join_subs(subs))


# IMAD cinematic saturation tracks (subrecord tags: first byte 0x11 = multiplier, 0x51 = additive, then "IAD"): the
# first byte indexes the record's tracks in xEdit's order (HDR, bloom, then cinematic saturation at 0x11). Which
# track is which was read from the layout, not seen in game: unverified.
IMAD_SAT_MUL, IMAD_SAT_ADD = "IAD", "QIAD"


def build_imad_comedown(edid):
    """Black-and-white comedown: ParalyzeMassImod with no blur, no tint / fade colour (alpha 0), saturation multiplied
    by 0 (and nothing added), and a duration of COMEDOWN_IMOD_SECONDS instead of 6 s."""
    subs = template(format(IMOD_DEPRESSANT_SOURCE, "08X"), "IMAD")
    replace(subs, "EDID", esp.zstr(edid))
    zero_blur(subs)
    for tag in ("TNAM", "NAM3"):  # colour keys: time, r, g, b, strength
        data = dict(subs)[tag]
        floats = list(struct.unpack(f"<{len(data) // 4}f", data))
        floats[4::5] = [0.0] * len(floats[4::5])
        replace(subs, tag, struct.pack(f"<{len(floats)}f", *floats))
    for tag in (IMAD_SAT_MUL, IMAD_SAT_ADD):
        data = dict(subs)[tag]
        floats = list(struct.unpack(f"<{len(data) // 4}f", data))
        floats[1::2] = [0.0] * len(floats[1::2])
        replace(subs, tag, struct.pack(f"<{len(floats)}f", *floats))
    dnam = dict(subs)["DNAM"]
    replace(subs, "DNAM", patch(dnam, 4, "<f", COMEDOWN_IMOD_SECONDS))
    return esp.record(b"IMAD", ids[edid], esp.join_subs(subs))


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
        "IMAD": [build_imad(IMAD_VELVET), build_imad_comedown(IMAD_COMEDOWN)],
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
    lines += ["", "    // Vanilla image space modifiers (Skyrim.esm, load order index 0) the plugin triggers (Velvet Haze uses HighrimImodVelvet)."]
    for name, formid in IMODS:
        lines.append(f"    inline constexpr std::uint32_t {name} = 0x{formid:06X};")
    lines += ["}", ""]
    return "\n".join(lines)


def main(argv):
    out_esp = argv[1] if len(argv) > 1 else os.path.join(ROOT, "package", "Data", PLUGIN_NAME)
    out_header = argv[2] if len(argv) > 2 else os.path.join(ROOT, "src", "Forms.h")
    data, forms = build()
    with open(out_esp, "wb") as handle:
        handle.write(data)
    text = forms_header(forms).encode("utf-8")
    try:
        with open(out_header, "rb") as handle:
            unchanged = handle.read().replace(b"\r\n", b"\n") == text
    except OSError:
        unchanged = False
    if not unchanged:  # an unchanged header keeps its timestamp, so the build's post-step does not make the DLL look stale
        with open(out_header, "wb") as handle:
            handle.write(text)
    print(f"Wrote {out_esp} ({len(data)} bytes) and {out_header} ({len(forms)} form IDs)")


if __name__ == "__main__":
    main(sys.argv)
