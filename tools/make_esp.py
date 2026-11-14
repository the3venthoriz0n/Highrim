"""Generates package/Data/Highrim.esp (light/ESL plugin) and src/Forms.h from vanilla templates.

Every record except the MCM quest is a clone of a vanilla Skyrim.esm record (copied into
tools/vanilla_templates.json by tools/extract_templates.py) with fields changed: new EDID, inline name and
text strings (this plugin is not localized), new effects and values. No record layout is hand-invented.

Usage: python -I tools/make_esp.py [<output.esp> [<Forms.h>]]
Defaults: package/Data/Highrim.esp and src/Forms.h next to this repository's tools folder.

Records, by fixed EDID (form IDs: the 0.1.0 records are pinned in tools/pinned_form_ids.json;
new ones are allocated in this order after the highest pinned ID, see allocate_ids):
  (Task 15: Rimerush is rebuilt. Its potion gives +10 Health / Magicka / Stamina and +10 to all 18 skills for 120 s
   (HighrimFxRushHealth / Magicka, HighrimFxRimerushStamina reused, 18 HighrimFxRushSkill<Skill>); its stack abilities give
   +5 per level to the same values (HighrimAbHealthUp / MagickaUp, HighrimAbStaminaUp, 18 HighrimAbRushSkill<Skill>). The old
   speed and regen records (HighrimFxRimerushSpeed / Regen, HighrimAbSpeedUp / RegenUp) stay in the file, unused: pinned.)
  (Task 18: Rimerush buffs go to 16 skills (not Alchemy, not Enchanting; their Fx / Ab records stay in the file, unused: pinned),
   the potion and the stack abilities get "Faster Attacks" (Weapon Speed Multiplier), the high has no screen effect, every text is
   condensed, the Rimerush drain can lower the maximums through the HighrimRavage spell, and "Addicted" and the withdrawal tiers
   have timed versions (HighrimTimedAddicted<Drug>, HighrimTimedWithdrawal1..3 and fixed-length variants V0..V6 of each) next to the
   old abilities, which stay in the file for saves made before.)
  KYWD  HighrimDrug                                 keyword on all three drugs (for the later dealer feature)
  MGEF  HighrimFxRimerushSpeed / Stamina / Regen    Rimerush effects (clones of potion effects)
        HighrimFxVelvetResist / VelvetSlow          Velvet Haze effects
        HighrimFxDetectLife                         one Detect Life effect with no conditions (no longer on Dreamcap: the plugin casts vanilla Detect Life itself; kept because its ID is in use)
        HighrimFxRegenDown                          stamina regen change, shared by comedowns (non-detrimental clone, negative magnitude)
        HighrimAbRegenDown / AbStaminaDown / AbHealRateDown   constant-effect clones for withdrawal/hangover
        HighrimAbSpeedUp / AbStaminaUp / AbRegenUp / AbResistUp / AbSlow   constant-effect clones for the dose stacks
        HighrimFxHighlyAddictive                    descriptor on every potion (magnitude 0, no gameplay effect)
        HighrimFxVelvetHealRate / VelvetCarry       Velvet Haze potion effects (the vanilla Invisibility effect is referenced, not cloned)
        HighrimAbHealRateUp / AbCarryUp             Velvet Haze stack effects
        HighrimAbAddicted                           the harmless effect of the Addicted abilities
        HighrimAbShakySpeech / AbShakySneak         Speech / Sneak -999 while high
        HighrimAbSkill<Skill>                       18 withdrawal effects, one per skill (tier penalty -10 / -20 / -30)
        HighrimAbOverdose<Low|Elevated|High>        the harmless effect (magnitude 0) of the three Overdose Risk abilities (Task 16)
        HighrimFxAllSkillsUp / AbAllSkillsUp / AbAllSkillsDown   Task 17: the one visible "All Skills Boosted" / "All Skills Reduced"
                                                    line that stands for the 54 hidden per-skill effects (Hide in UI flag)
  SPEL  HighrimShaky                                "Shaky and Suspicious": Speech and Sneak 0 while high
        HighrimAddicted<Rimerush|VelvetHaze|Dreamcap>   "Addicted: <drug>" abilities (dependency level 10 or more)
        HighrimOverdose<Low|Elevated|High>          "Overdose Risk: <tier>" abilities, one at a time while the dose total is inside the window
        HighrimComedownRimerush / VelvetHaze / Dreamcap   60 s fire-and-forget spells the plugin casts at the end of a high
        HighrimWithdrawal1 / 2 / 3                  withdrawal abilities (type Ability), one set for all drug types
        HighrimHangover                             ability the plugin adds after a blackout and removes after a game day
        HighrimStack<Rimerush|VelvetHaze|Dreamcap>1..3   dose stack abilities (type Ability), one at a time per drug
        HighrimBlurPulse                            8 s blur the plugin casts now and then (withdrawal tier 2, hangover)
        HighrimLostGear<Hold>                       nine abilities (one per hold) that say where the player's things are
        HighrimAbLostGear                           the (harmless) effect those abilities carry
  IMAD  HighrimImodVelvet                           Velvet Haze screen effect: ParalyzeMassImod with the blur tracks zeroed
        HighrimImodComedown                         black-and-white comedown look: the same record, no blur, no tint, saturation 0, 60 s
        HighrimImodOverdose                         overdose warning: LowHealthImod (the vanilla low-health look) with every blur track zeroed,
                                                    a stronger tint and a 4 s run (the plugin re-triggers it)
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
# The hold's capital city, which is all the notes and the Lost Gear ability name (same table as Core::CapitalName).
CAPITALS = {"Whiterun": "Whiterun", "Eastmarch": "Windhelm", "Rift": "Riften", "Haafingar": "Solitude",
            "Hjaalmarch": "Morthal", "Reach": "Markarth", "Falkreath": "Falkreath", "Pale": "Dawnstar",
            "Winterhold": "Winterhold"}


def note_text(hold):
    return ("Last night is a blur. Shouting. Guards. A cold floor, maybe a cell. Your things might be somewhere in "
            f"{CAPITALS[hold]}... if the guards kept them.")

# Vanilla form IDs used by reference (all confirmed in Skyrim.esm by tests/test_esp.py).
GIANT_LICHEN, GLOWING_MUSHROOM, LAVENDER, HANGING_MOSS, SWAMP_FUNGAL_POD = (
    0x0007E8C1, 0x0007EE01, 0x00045C28, 0x00057F91, 0x0007E8B7)
COOKPOT_KEYWORD = 0x000A5CB3
VENDOR_ITEM_POTION = 0x0008CDEC
MAGIC_ALCH_BENEFICIAL = 0x000F8A4E
# Screen effects the plugin triggers itself (vanilla IMAD records in Skyrim.esm). Picked by name only; what they look like
# in game is unverified. Candidates considered are listed in the README. Task 18: Rimerush (the stimulant) has none during
# the high any more (QuickReflexesImod, 0010C761, is no longer used); only the comedown look follows it.
IMOD_DEPRESSANT_SOURCE = 0x00031060  # ParalyzeMassImod: Velvet Haze's own IMAD (HighrimImodVelvet) is cloned from it
IMOD_HALLUCINOGEN = 0x000C1966  # DA13PeryiteVisionImod: strong tinted vision
IMODS = [("kImodHallucinogen", IMOD_HALLUCINOGEN)]
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
DETECT_LIFE_TEXT = "Living things are visible through walls within <area> feet for <dur> seconds."  # unused: the plugin casts vanilla Detect Life
GOLD = 0x0000000F             # Gold001
RECIPE_GOLD = 1000            # every cooking pot recipe costs exactly this much gold and nothing else
IMAD_COMEDOWN = "HighrimImodComedown"
# Task 16: the screen warning at the Elevated and High overdose risk tiers. LowHealthImod is the vanilla low-health look
# (a red tint at the edges of the effect plus blur). Picked by name and by reading its tracks, not from seeing it in game.
IMOD_OVERDOSE_SOURCE = 0x0010C445
IMAD_OVERDOSE = "HighrimImodOverdose"
OVERDOSE_IMOD_SECONDS = 4.0     # the vanilla record runs 1 s; the plugin re-triggers it about every 10 s ("pulsing")
OVERDOSE_TINT_ALPHA = 0.35      # the vanilla peak tint alpha is 0.098, too faint at strength 0.4: raised (unverified in game)
COMEDOWN_IMOD_SECONDS = 60.0  # duration written into the comedown IMAD (the plugin also stops it by itself)
ACTOR_VALUE_CARRY_WEIGHT = 32
# Task 17: the Hide in UI flag of a magic effect (EffectSetting::EffectSettingData::Flag::kHideInUI = 1 << 15). Checked
# against Skyrim.esm: 170 vanilla effects have it (MS04RewardEffectNoDisplay, AbVampireDamageHealth ...); bit 18 (0x40000)
# is only set on the Bound weapon effects. Whether the game really leaves flagged effects out of tooltips is unverified.
MGEF_HIDE_IN_UI = 0x00008000
# The descriptor effects need a magnitude (the buff / penalty, for <mag> in the text) but no gameplay: they change
# Variable10 (actor value 77), which no vanilla magic effect uses (Skyrim.esm checked: Variable01..10 never appear).
ACTOR_VALUE_VARIABLE10 = 77
ACTOR_VALUE_HEALTH, ACTOR_VALUE_MAGICKA, ACTOR_VALUE_STAMINA = 24, 25, 26

# Template form IDs (see tools/extract_templates.py).
T_FORT_STAMINA, T_FORT_STAMINA_RATE, T_SAP_SLOW = "0003EAF9", "0003EB08", "000E3CBA"
T_AB_FORT_STAMINA = "00091F77"
T_ALCH, T_BOOK, T_ABILITY, T_SPELL, T_COBJ, T_KYWD = "00057A7A", "000E1A9F", "0010FB30", "0005AD5C", "000F431F", "0008CDEC"

ACTOR_VALUE_SPEED_MULT = 30
ACTOR_VALUE_WEAPON_SPEED_MULT = 85   # "Weapon Speed Multiplier" (attack speed); vanilla effects use the whole multiplier (1.2 = 20% faster)
ACTOR_VALUE_DAMAGE_RESIST = 39
ACTOR_VALUE_STAMINA_RATE = 0x9D   # the value AbDamageStaminaRate and AlchFortifyStaminaRate change
ACTOR_VALUE_HEAL_RATE = 0x9B      # the value VampireAbDamageHealRate changes

# Dose stacks: stack abilities come in levels 1..3 (stack 2, 3, 4 and beyond), on top of the potion's own effects.
STACK_LEVELS = 3
HIGH_SECONDS = 120   # how long a potion's own effects last

# Potion base magnitudes. Task 9 raised them about 35% (was: speed 25, stamina 50, regen 100; resist 150, slow 20).
# Task 14: Velvet Haze no longer adds armour (HighrimFxVelvetResist stays in the plugin, pinned, unused); it gives rapid
# health regeneration, more carry weight and invisibility. "regen" is a percentage of the base health regeneration
# (actor value Health Rate Mult, 155, the one the vampire / withdrawal abilities lower by 50): +500 means the
# player regenerates 6 times as fast as normal, a clearly rapid value (the rate is cut in combat by a game setting,
# so the real in-combat effect is unverified). Carry weight is in points. Invisibility has no magnitude.
RIMERUSH_BASE = {"speed": 34, "stamina": 68, "regen": 135}   # Task 9 values, no longer used by the potion (Task 15)
# Task 15: what Rimerush gives while high (Health, Magicka, Stamina and every skill), and per stack level.
RIMERUSH_BUFF = 10
RIMERUSH_STACK_BUFF = 5
# Task 18: Rimerush attack speed. The Weapon Speed Multiplier has a base of 0 (CommonLibSSE-NG: kBaseValueIsAlwaysZero) and the
# vanilla effects that change it write the whole multiplier as the magnitude (PerkDualFlurry 1.2 / 1.35 = 20% / 35% faster,
# PerkQuickShot 1.3), so the potion's magnitude is 1 + percent / 100 and each stack level adds the stack percent on top (modifiers
# add up). Unverified in game: if the value turns out to start at 1.0 the potion would be +120%.
RIMERUSH_ATTACK_SPEED_PERCENT = 20
RIMERUSH_ATTACK_SPEED_STACK_PERCENT = 5
RIMERUSH_ATTACK_SPEED = 1 + RIMERUSH_ATTACK_SPEED_PERCENT / 100
RIMERUSH_ATTACK_SPEED_STACK = tuple(round(RIMERUSH_ATTACK_SPEED_STACK_PERCENT * n / 100, 2) for n in (1, 2, 3))
# Task 18: the ravage (maximum reduction) spell and the timed addiction / withdrawal spells.
RAVAGE_SECONDS = 99999        # like Survival Mode's penalty spells (a long timed spell the plugin dispels itself)
TIMED_PLACEHOLDER_SECONDS = 3600   # the duration written in the base timed spells; the plugin sets the live effect's duration
# Fixed-length variants of every timed spell (real seconds, the fallback if the live duration cannot be set): same table as
# Core::kTimedVariantSeconds (tests/test_esp.py checks the two agree).
TIMED_VARIANT_SECONDS = (600, 3600, 10800, 28800, 64800, 129600, 259200)
VELVET_BASE = {"slow": 27, "regen": 500, "carry": 150}
VELVET_STACK_REGEN = (100, 200, 300)   # more health regeneration (percent of the base rate) per stack level
VELVET_STACK_CARRY = (50, 100, 150)    # more carry weight per stack level
ACTOR_VALUE_HEAL_RATE_MULT = 0x9B      # Health Rate Mult (155), same value as ACTOR_VALUE_HEAL_RATE above
INVISIBILITY_EFFECT = 0x0003EB3D       # AlchInvisibillity (Skyrim.esm): the effect of the vanilla Potion of Invisibility

# Task 14: skills. Every withdrawal tier lowers all 18 skills by a flat amount; a high sets Speech and Sneak to 0.
WITHDRAWAL_SKILL_PENALTY = {1: -10, 2: -20, 3: -30}
SHAKY_SKILL_PENALTY = -999             # "Shaky and Suspicious": large enough to bring any skill to 0
# (name, actor value) in the order of the game's actor value table
SKILLS = [("One-handed", 6), ("Two-handed", 7), ("Archery", 8), ("Block", 9), ("Smithing", 10), ("Heavy Armor", 11),
          ("Light Armor", 12), ("Pickpocket", 13), ("Lockpicking", 14), ("Sneak", 15), ("Alchemy", 16), ("Speech", 17),
          ("Alteration", 18), ("Conjuration", 19), ("Destruction", 20), ("Illusion", 21), ("Restoration", 22),
          ("Enchanting", 23)]
assert len(SKILLS) == 18
# Task 18: Rimerush boosts 16 skills (Alchemy and Enchanting are left out); the withdrawal penalty stays on all 18.
RUSH_SKILLS = [(n, av) for n, av in SKILLS if n not in ("Alchemy", "Enchanting")]
assert len(RUSH_SKILLS) == 16


def skill_edid(name):
    return "HighrimAbSkill" + "".join(w.capitalize() for w in name.replace("-", " ").split())


def wd_skill_edid(name):
    """HighrimFxWdSkill<Skill>: the timed withdrawal's hidden per-skill effect."""
    return "HighrimFxWdSkill" + "".join(w.capitalize() for w in name.replace("-", " ").split())


def rush_skill_edid(name, kind):
    """HighrimFxRushSkill<Skill> (potion effect) or HighrimAbRushSkill<Skill> (stack ability effect)."""
    return f"Highrim{kind}RushSkill" + "".join(w.capitalize() for w in name.replace("-", " ").split())


# Task 16: overdose risk tiers (key, ability name, description). Same text as Core::RiskTierName / RiskTierDescription.
OVERDOSE_TIERS = [
    ("Low", "Overdose Risk: Low", "You are handling it, for now."),
    ("Elevated", "Overdose Risk: Elevated", "One more dose could knock you out."),
    ("High", "Overdose Risk: High", "Another dose may put you on the floor."),
]

DRUG_NAMES = {"Rimerush": "Rimerush", "VelvetHaze": "Velvet Haze", "Dreamcap": "Dreamcap"}
ADDICTED_TEXT = "You depend on {drug}."                          # the old lasting ability (kept for saves)
ADDICTED_TIMED_TEXT = "Dependent on {drug} until the timer ends."  # the timed spell: the timer is the time until the dependency is below 10
HIGHLY_ADDICTIVE_TEXT = "Highly addictive: withdrawal follows when you stop."

ALL_SKILLS_UP_TEXT = "All skills except Alchemy and Enchanting +<mag>."
ALL_SKILLS_DOWN_TEXT = "All skills changed by <mag> in withdrawal."
ATTACK_SPEED_POTION_TEXT = f"Weapon attacks {RIMERUSH_ATTACK_SPEED_PERCENT}% faster for <dur> seconds."
ATTACK_SPEED_STACK_TEXT = "Weapon attacks are faster."   # the spell text says how much per level (<pct>, see PLAIN)

# (EDID, name, description, template, overrides)
MGEFS = [
    ("HighrimFxRimerushSpeed", "Rush Speed", "Movement speed +<mag>% for <dur> seconds.",
     T_FORT_STAMINA, {"av": ACTOR_VALUE_SPEED_MULT, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimFxRimerushStamina", "Rush Stamina", "Stamina +<mag> for <dur> seconds.",
     T_FORT_STAMINA, {}),
    ("HighrimFxRimerushRegen", "Rush Recovery", "Stamina regeneration +<mag>% for <dur> seconds.",
     T_FORT_STAMINA_RATE, {}),
    ("HighrimFxVelvetResist", "Velvet Armor", "Armor +<mag> for <dur> seconds.",
     T_FORT_STAMINA, {"av": ACTOR_VALUE_DAMAGE_RESIST, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimFxVelvetSlow", "Velvet Drag", "Movement is <mag>% slower for <dur> seconds.", T_SAP_SLOW, {}),
    # Comedown, withdrawal and hangover effects are NOT clones of the detrimental (Damage ...) effects any more:
    # those carry the Hostile and Detrimental flags. They are Fortify-style clones with a negative magnitude, no
    # hostile/detrimental flag and no harmful keyword (see the README on Cure Disease).
    ("HighrimFxRegenDown", "Slowed Recovery", "Stamina regeneration changed by <mag>% for <dur> seconds.",
     T_FORT_STAMINA_RATE, {"drop": ["KSIZ", "KWDA"]}),
    ("HighrimFxDetectLife", "Detect Life", DETECT_LIFE_TEXT, "001019D8", {"drop": ["CTDA"]}),
    ("HighrimAbRegenDown", "Cravings", "Stamina regeneration changed by <mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_STAMINA_RATE}),
    ("HighrimAbStaminaDown", "Shakes", "Stamina changed by <mag>.", T_AB_FORT_STAMINA, {}),
    ("HighrimAbHealRateDown", "Slow Healing", "Health regeneration changed by <mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_HEAL_RATE}),
    # Dose stack effects: constant-effect (ability) clones of AbFortifyStamina with the actor value changed.
    ("HighrimAbSpeedUp", "Surge Speed", "Movement speed +<mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_SPEED_MULT}),
    ("HighrimAbStaminaUp", "Surge Stamina", "Stamina +<mag>.", T_AB_FORT_STAMINA, {}),
    ("HighrimAbRegenUp", "Surge Recovery", "Stamina regeneration +<mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_STAMINA_RATE}),
    ("HighrimAbResistUp", "Heavy Armor", "Armor +<mag>.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_DAMAGE_RESIST}),
    ("HighrimAbSlow", "Dragging Limbs", "Movement speed changed by <mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_SPEED_MULT}),
    # The effect the Lost Gear abilities carry. An ability needs at least one effect to appear in Active Effects, so
    # it is +1 carry weight (harmless); the spell's own description is what tells the player where the gear is.
    ("HighrimAbLostGear", "Lost Gear", "Your things may be with the guards.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_CARRY_WEIGHT}),
    # Task 14. The descriptor every potion carries (magnitude 0: a carry weight change of nothing).
    ("HighrimFxHighlyAddictive", "Highly Addictive", HIGHLY_ADDICTIVE_TEXT, T_FORT_STAMINA,
     {"av": ACTOR_VALUE_CARRY_WEIGHT, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimFxVelvetHealRate", "Velvet Regen", "Health regeneration +<mag>% for <dur> seconds.",
     T_FORT_STAMINA_RATE, {"av": ACTOR_VALUE_HEAL_RATE_MULT, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimFxVelvetCarry", "Velvet Strength", "Carry weight +<mag> for <dur> seconds.",
     T_FORT_STAMINA, {"av": ACTOR_VALUE_CARRY_WEIGHT, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimAbHealRateUp", "Surge Regen", "Health regeneration +<mag>%.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_HEAL_RATE_MULT}),
    ("HighrimAbCarryUp", "Surge Strength", "Carry weight +<mag>.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_CARRY_WEIGHT}),
    # Addicted abilities carry this: no effect on the game (magnitude 0), it only gives the ability a line in Active Effects.
    ("HighrimAbAddicted", "Addicted", "You depend on this drug.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_CARRY_WEIGHT}),
    # "Shaky and Suspicious": Speech and Sneak go to 0 while high.
    ("HighrimAbShakySpeech", "Shaky Speech", "Speech is 0 while high.", T_AB_FORT_STAMINA, {"av": 17}),
    ("HighrimAbShakySneak", "Suspicious Sneak", "Sneak is 0 while high.", T_AB_FORT_STAMINA, {"av": 15}),
    # Task 15: Rimerush buffs. Potion effects (timed) and ability effects (stack levels).
    ("HighrimFxRushHealth", "Rush Vitality", "Health +<mag> for <dur> seconds.", T_FORT_STAMINA,
     {"av": ACTOR_VALUE_HEALTH, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimFxRushMagicka", "Rush Focus", "Magicka +<mag> for <dur> seconds.", T_FORT_STAMINA,
     {"av": ACTOR_VALUE_MAGICKA, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimAbHealthUp", "Surge Vitality", "Health +<mag>.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_HEALTH}),
    ("HighrimAbMagickaUp", "Surge Focus", "Magicka +<mag>.", T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_MAGICKA}),
] + [
    (rush_skill_edid(name, "Fx"), f"Rush {name}", f"{name} +<mag> for <dur> seconds.", T_FORT_STAMINA,
     {"av": av, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL], "hide": True})
    for name, av in SKILLS
] + [
    (rush_skill_edid(name, "Ab"), f"Surge {name}", f"{name} +<mag>.", T_AB_FORT_STAMINA,
     {"av": av, "hide": True})
    for name, av in SKILLS
] + [
    # One per skill, used by all three withdrawal abilities (the magnitude is the tier's penalty).
    (skill_edid(name), f"Withdrawal: {name}", f"{name} changed by <mag> in withdrawal.",
     T_AB_FORT_STAMINA, {"av": av, "hide": True})
    for name, av in SKILLS
] + [
    # Task 17: one visible descriptor per group of hidden per-skill effects (potion and ability versions are separate
    # clones, like every other Fx / Ab pair). The magnitude of the effect is the buff or penalty, for <mag> in the text.
    ("HighrimFxAllSkillsUp", "All Skills Boosted", ALL_SKILLS_UP_TEXT, T_FORT_STAMINA,
     {"av": ACTOR_VALUE_VARIABLE10, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimAbAllSkillsUp", "All Skills Boosted", ALL_SKILLS_UP_TEXT, T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_VARIABLE10, "show": True}),
    ("HighrimAbAllSkillsDown", "All Skills Reduced", ALL_SKILLS_DOWN_TEXT, T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_VARIABLE10, "show": True}),
] + [
    # Task 16: the three Overdose Risk abilities carry one harmless effect each (carry weight, magnitude 0): an ability needs
    # an effect to be listed in Active Effects. The effect text is the plain description.
    (f"HighrimAbOverdose{key}", name, desc, T_AB_FORT_STAMINA, {"av": ACTOR_VALUE_CARRY_WEIGHT})
    for key, name, desc in OVERDOSE_TIERS
] + [
    # Task 18. Rimerush attack speed (potion and stack ability): the Weapon Speed Multiplier (see RIMERUSH_ATTACK_SPEED).
    ("HighrimFxRushAttackSpeed", "Faster Attacks", ATTACK_SPEED_POTION_TEXT, T_FORT_STAMINA,
     {"av": ACTOR_VALUE_WEAPON_SPEED_MULT, "assoc": 0, "keywords": [MAGIC_ALCH_BENEFICIAL]}),
    ("HighrimAbAttackSpeedUp", "Faster Attacks", ATTACK_SPEED_STACK_TEXT, T_AB_FORT_STAMINA,
     {"av": ACTOR_VALUE_WEAPON_SPEED_MULT, "show": True}),
    # The ravage effects (Rimerush withdrawal): Peak Value Modifier clones of Fortify Stamina with a negative magnitude that
    # the plugin writes before it casts HighrimRavage; they lower the MAXIMUM of the bar, as Survival Mode's penalties do.
    ("HighrimFxRavageHealth", "Drained Health", "Withdrawal lowers your maximum health.", T_FORT_STAMINA,
     {"av": ACTOR_VALUE_HEALTH, "assoc": 0, "drop": ["KSIZ", "KWDA"]}),
    ("HighrimFxRavageMagicka", "Drained Magicka", "Withdrawal lowers your maximum magicka.", T_FORT_STAMINA,
     {"av": ACTOR_VALUE_MAGICKA, "assoc": 0, "drop": ["KSIZ", "KWDA"]}),
    ("HighrimFxRavageStamina", "Drained Stamina", "Withdrawal lowers your maximum stamina.", T_FORT_STAMINA,
     {"av": ACTOR_VALUE_STAMINA, "assoc": 0, "drop": ["KSIZ", "KWDA"]}),
    # Timed addiction and withdrawal (spells, so the game's Active Effects timer counts down): potion-style clones of the
    # ability effects above, no hostile / detrimental flags and no keywords (as the comedown effects). Like the abilities, only the
    # descriptor ("All Skills Reduced") is listed in Active Effects; the others are hidden (Hide in UI).
    ("HighrimFxAddicted", "Addicted", "You depend on this drug until the timer ends.", T_FORT_STAMINA,
     {"av": ACTOR_VALUE_CARRY_WEIGHT, "assoc": 0, "drop": ["KSIZ", "KWDA"]}),
    ("HighrimFxWdRegenDown", "Cravings", "Stamina regeneration changed by <mag>%.", T_FORT_STAMINA_RATE,
     {"hide": True, "drop": ["KSIZ", "KWDA"]}),
    ("HighrimFxWdStaminaDown", "Shakes", "Stamina changed by <mag>.", T_FORT_STAMINA, {"hide": True, "drop": ["KSIZ", "KWDA"]}),
    ("HighrimFxWdHealRateDown", "Slow Healing", "Health regeneration changed by <mag>%.", T_FORT_STAMINA_RATE,
     {"av": ACTOR_VALUE_HEAL_RATE, "hide": True, "drop": ["KSIZ", "KWDA"]}),
    ("HighrimFxWdAllSkillsDown", "All Skills Reduced", ALL_SKILLS_DOWN_TEXT, T_FORT_STAMINA,
     {"av": ACTOR_VALUE_VARIABLE10, "assoc": 0, "drop": ["KSIZ", "KWDA"]}),
] + [
    (wd_skill_edid(name), f"Withdrawal: {name}", f"{name} changed by <mag> in withdrawal.", T_FORT_STAMINA,
     {"av": av, "assoc": 0, "hide": True, "drop": ["KSIZ", "KWDA"]})
    for name, av in SKILLS
]

# Plain-words text for the spell descriptions of effects whose magnitude is negative (<neg> is the absolute value).
# The effect's own text (above) is what the engine shows for the effect; this is what the spell's tooltip shows.
PLAIN = {
    "HighrimFxRegenDown": "Stamina regeneration <neg>% slower for <dur> seconds.",
    "HighrimAbRegenDown": "Stamina regeneration <neg>% slower.",
    "HighrimAbStaminaDown": "Maximum stamina <neg> lower.",
    "HighrimAbHealRateDown": "Health regeneration <neg>% slower.",
    "HighrimAbSlow": "Movement <neg>% slower.",
    "HighrimAbAttackSpeedUp": "Weapon attacks <pct>% faster.",
    "HighrimFxWdRegenDown": "Stamina regeneration <neg>% slower.",
    "HighrimFxWdStaminaDown": "Maximum stamina <neg> lower.",
    "HighrimFxWdHealRateDown": "Health regeneration <neg>% slower.",
}
PLAIN["HighrimAbAllSkillsDown"] = "All skills <neg> lower in withdrawal."
PLAIN["HighrimFxWdAllSkillsDown"] = PLAIN["HighrimAbAllSkillsDown"]
PLAIN.update({skill_edid(name): f"{name} <neg> lower in withdrawal." for name, _ in SKILLS})
PLAIN.update({wd_skill_edid(name): f"{name} <neg> lower in withdrawal." for name, _ in SKILLS})


def fx(edid):
    return ids[edid]


def effect(mgef, magnitude, duration, area=0, conditions=()):
    return {"mgef": mgef, "mag": magnitude, "area": area, "dur": duration, "ctda": list(conditions)}


# Per drug: (EDID prefix, display name, [(MGEF EDID, magnitudes for stack level 1, 2, 3)]). Each effect appears once
# per ability and each level is strictly stronger than the one before. Dreamcap has no stat effect of its own, so
# its stack is a clumsy trip: slowed movement and slowed stamina recovery (a design call, see the README).
STACKS = [
    ("HighrimStackRimerush", "Rimerush Surge",
     [("HighrimAbHealthUp", tuple(RIMERUSH_STACK_BUFF * n for n in (1, 2, 3))),
      ("HighrimAbMagickaUp", tuple(RIMERUSH_STACK_BUFF * n for n in (1, 2, 3))),
      ("HighrimAbStaminaUp", tuple(RIMERUSH_STACK_BUFF * n for n in (1, 2, 3))),
      ("HighrimAbAllSkillsUp", tuple(RIMERUSH_STACK_BUFF * n for n in (1, 2, 3))),
      ("HighrimAbAttackSpeedUp", RIMERUSH_ATTACK_SPEED_STACK)]
     + [(rush_skill_edid(name, "Ab"), tuple(RIMERUSH_STACK_BUFF * n for n in (1, 2, 3))) for name, _ in RUSH_SKILLS]),
    ("HighrimStackVelvetHaze", "Velvet Weight",
     [("HighrimAbHealRateUp", VELVET_STACK_REGEN), ("HighrimAbCarryUp", VELVET_STACK_CARRY),
      ("HighrimAbSlow", (-8, -16, -24))]),
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


def skill_effects(tier):
    """The visible descriptor, then the 18 hidden per-skill effects (all four carry the tier's penalty)."""
    return [effect(fx("HighrimAbAllSkillsDown"), WITHDRAWAL_SKILL_PENALTY[tier], 0)] + [effect(fx(skill_edid(name)), WITHDRAWAL_SKILL_PENALTY[tier], 0) for name, _ in SKILLS]


TIMED_WITHDRAWAL_EFFECTS = {   # tier -> [(effect EDID, magnitude)], then the skills (all carry the tier's penalty)
    1: [("HighrimFxWdRegenDown", -20)],
    2: [("HighrimFxWdRegenDown", -35), ("HighrimFxWdStaminaDown", -25)],
    3: [("HighrimFxWdRegenDown", -50), ("HighrimFxWdStaminaDown", -40), ("HighrimFxWdHealRateDown", -50)],
}
TIMED_WITHDRAWAL_NAMES = {1: "Withdrawal I: Cravings", 2: "Withdrawal II: Shakes", 3: "Withdrawal III: Sickness"}
TIMED_VARIANTS = len(TIMED_VARIANT_SECONDS)


def timed_addicted_edid(key, variant=None):
    return f"HighrimTimedAddicted{key}" + ("" if variant is None else f"V{variant}")


def timed_withdrawal_edid(tier, variant=None):
    return f"HighrimTimedWithdrawal{tier}" + ("" if variant is None else f"V{variant}")


def timed_defs():
    """The timed spells: base (placeholder duration, the plugin sets the live one) and variants V0.. (fixed real seconds)."""
    out = []
    for variant in [None] + list(range(TIMED_VARIANTS)):
        secs = TIMED_PLACEHOLDER_SECONDS if variant is None else TIMED_VARIANT_SECONDS[variant]
        for key, name in DRUG_NAMES.items():
            out.append((timed_addicted_edid(key, variant), f"Addicted: {name}", "spell", [effect(fx("HighrimFxAddicted"), 0, secs)]))
        for tier in (1, 2, 3):
            pen = WITHDRAWAL_SKILL_PENALTY[tier]
            effects = [effect(fx(edid), mag, secs) for edid, mag in TIMED_WITHDRAWAL_EFFECTS[tier]]
            effects += [effect(fx("HighrimFxWdAllSkillsDown"), pen, secs)] + [effect(fx(wd_skill_edid(n)), pen, secs) for n, _ in SKILLS]
            out.append((timed_withdrawal_edid(tier, variant), TIMED_WITHDRAWAL_NAMES[tier], "spell", effects))
    return out


def spell_defs():
    """(EDID, name, kind, effects) with kind 'ability' or 'spell'."""
    return [
        # Task 18: the ravage spell; its three magnitudes are 0 here, the plugin writes them before every cast.
        ("HighrimRavage", "Withdrawal Drain", "spell",
         [effect(fx("HighrimFxRavageHealth"), 0, RAVAGE_SECONDS), effect(fx("HighrimFxRavageMagicka"), 0, RAVAGE_SECONDS),
          effect(fx("HighrimFxRavageStamina"), 0, RAVAGE_SECONDS)]),
    ] + timed_defs() + [
        ("HighrimComedownRimerush", "Rimerush Crash", "spell", [effect(fx("HighrimFxRegenDown"), -60, 60)]),
        ("HighrimComedownVelvetHaze", "Velvet Crash", "spell", [effect(fx("HighrimFxRegenDown"), -40, 60)]),
        ("HighrimComedownDreamcap", "Dreamcap Crash", "spell", [effect(DRUGGED_EFFECT, 0, 60)]),
        ("HighrimWithdrawal1", "Withdrawal I: Cravings", "ability",
         [effect(fx("HighrimAbRegenDown"), -20, 0)] + skill_effects(1)),
        ("HighrimWithdrawal2", "Withdrawal II: Shakes", "ability",
         [effect(fx("HighrimAbRegenDown"), -35, 0), effect(fx("HighrimAbStaminaDown"), -25, 0)] + skill_effects(2)),
        ("HighrimWithdrawal3", "Withdrawal III: Sickness", "ability",
         [effect(fx("HighrimAbRegenDown"), -50, 0), effect(fx("HighrimAbStaminaDown"), -40, 0),
          effect(fx("HighrimAbHealRateDown"), -50, 0)] + skill_effects(3)),
        ("HighrimHangover", "Hangover", "ability",
         [effect(fx("HighrimAbStaminaDown"), -30, 0), effect(fx("HighrimAbRegenDown"), -25, 0)]),
        ("HighrimBlurPulse", "Blurred Vision", "spell", [effect(DRUGGED_EFFECT, 0, 8)]),
    ] + stack_spell_defs() + [
        (f"HighrimLostGear{hold}", f"Lost Gear: {CAPITALS[hold]}?", "ability", [effect(fx("HighrimAbLostGear"), 1, 0)])
        for hold in HOLDS] + [
        ("HighrimShaky", "Shaky and Suspicious", "ability",
         [effect(fx("HighrimAbShakySpeech"), SHAKY_SKILL_PENALTY, 0), effect(fx("HighrimAbShakySneak"), SHAKY_SKILL_PENALTY, 0)])
    ] + [(f"HighrimAddicted{key}", f"Addicted: {name}", "ability", [effect(fx("HighrimAbAddicted"), 0, 0)])
         for key, name in DRUG_NAMES.items()] + [
        (f"HighrimOverdose{key}", name, "ability", [effect(fx(f"HighrimAbOverdose{key}"), 0, 0)])
        for key, name, _ in OVERDOSE_TIERS]


# Spell descriptions that say more than the effect texts do (everything else is built from the effects' texts).
SPELL_EXTRA = {
    "HighrimWithdrawal2": " Vision blurs now and then.",
    "HighrimWithdrawal3": " Vision blurs now and then.",
    "HighrimHangover": " Vision blurs now and then. Lasts one game day.",
}
SPELL_EXTRA.update({timed_withdrawal_edid(t, v): SPELL_EXTRA[f"HighrimWithdrawal{t}"] for t in (2, 3)
                    for v in [None] + list(range(TIMED_VARIANTS))})
SPELL_TEXT = {"HighrimShaky": "Speech and Sneak are 0 while high."}
SPELL_TEXT.update({f"HighrimAddicted{key}": ADDICTED_TEXT.format(drug=name) for key, name in DRUG_NAMES.items()})
SPELL_TEXT.update({f"HighrimOverdose{key}": desc for key, _, desc in OVERDOSE_TIERS})
SPELL_TEXT.update({timed_addicted_edid(key, v): ADDICTED_TIMED_TEXT.format(drug=name) for key, name in DRUG_NAMES.items()
                   for v in [None] + list(range(TIMED_VARIANTS))})
SPELL_TEXT["HighrimRavage"] = "Withdrawal lowers your maximum health, magicka and stamina."
SPELL_TEXT.update({f"HighrimLostGear{hold}": f"The guards in {CAPITALS[hold]} may have your things." for hold in HOLDS})


def drug_defs():
    """(EDID, name, value, effects)."""
    high = HIGH_SECONDS
    return [
        ("HighrimRimerush", "Rimerush", 60,
         [effect(fx("HighrimFxRushHealth"), RIMERUSH_BUFF, high),
          effect(fx("HighrimFxRushMagicka"), RIMERUSH_BUFF, high),
          effect(fx("HighrimFxRimerushStamina"), RIMERUSH_BUFF, high),
          effect(fx("HighrimFxAllSkillsUp"), RIMERUSH_BUFF, high),
          effect(fx("HighrimFxRushAttackSpeed"), RIMERUSH_ATTACK_SPEED, high)]
         + [effect(fx(rush_skill_edid(name, "Fx")), RIMERUSH_BUFF, high) for name, _ in RUSH_SKILLS]
         + [effect(fx("HighrimFxHighlyAddictive"), 0, high)]),
        ("HighrimVelvetHaze", "Velvet Haze", 55,
         [effect(fx("HighrimFxVelvetSlow"), VELVET_BASE["slow"], high),
          effect(fx("HighrimFxVelvetHealRate"), VELVET_BASE["regen"], high),
          effect(fx("HighrimFxVelvetCarry"), VELVET_BASE["carry"], high),
          effect(INVISIBILITY_EFFECT, 0, high),
          effect(fx("HighrimFxHighlyAddictive"), 0, high)]),
        ("HighrimDreamcap", "Dreamcap", 70,
         [effect(DRUGGED_EFFECT, 0, high), effect(fx("HighrimFxHighlyAddictive"), 0, high)]),  # reveal life is cast by the plugin (vanilla Detect Life), not a potion effect
    ]


RECIPES = [  # (EDID, product EDID, [(ingredient, count)]): 1000 gold, nothing else (the 0.1.0 plant recipes are gone)
    ("HighrimRecipeRimerush", "HighrimRimerush", [(GOLD, RECIPE_GOLD)]),
    ("HighrimRecipeVelvetHaze", "HighrimVelvetHaze", [(GOLD, RECIPE_GOLD)]),
    ("HighrimRecipeDreamcap", "HighrimDreamcap", [(GOLD, RECIPE_GOLD)]),
]

KEYWORD_EDID = "HighrimDrug"
SPELL_EDIDS = ["HighrimComedownRimerush", "HighrimComedownVelvetHaze", "HighrimComedownDreamcap",
               "HighrimWithdrawal1", "HighrimWithdrawal2", "HighrimWithdrawal3", "HighrimHangover",
               "HighrimBlurPulse"] + STACK_EDIDS + [f"HighrimLostGear{hold}" for hold in HOLDS] + [
    "HighrimShaky"] + [f"HighrimAddicted{key}" for key in DRUG_NAMES] + [f"HighrimOverdose{key}" for key, _, _ in OVERDOSE_TIERS] + [
    "HighrimRavage"] + [timed_addicted_edid(key) for key in DRUG_NAMES] + [timed_withdrawal_edid(t) for t in (1, 2, 3)] + [
    timed_addicted_edid(key, v) for v in range(TIMED_VARIANTS) for key in DRUG_NAMES] + [
    timed_withdrawal_edid(t, v) for v in range(TIMED_VARIANTS) for t in (1, 2, 3)]
NOTE_EDIDS = ["HighrimNote" + hold for hold in HOLDS]
ALCH_EDIDS = ["HighrimRimerush", "HighrimVelvetHaze", "HighrimDreamcap"]
COBJ_EDIDS = [r[0] for r in RECIPES]
# Group order and the order form IDs are handed out in.
ORDER = [("KYWD", [KEYWORD_EDID]), ("MGEF", [m[0] for m in MGEFS]), ("SPEL", SPELL_EDIDS),
         ("BOOK", NOTE_EDIDS), ("ALCH", ALCH_EDIDS), ("COBJ", COBJ_EDIDS), ("IMAD", [IMAD_VELVET, IMAD_COMEDOWN, IMAD_OVERDOSE])]

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


MGEF_FLAGS_OFFSET = 0
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
    if overrides.get("hide"):
        data = patch(data, MGEF_FLAGS_OFFSET, "<I", struct.unpack_from("<I", data, MGEF_FLAGS_OFFSET)[0] | MGEF_HIDE_IN_UI)
    if overrides.get("show"):  # the AbFortifyStamina template already has Hide in UI set: clear it for a visible descriptor
        data = patch(data, MGEF_FLAGS_OFFSET, "<I", struct.unpack_from("<I", data, MGEF_FLAGS_OFFSET)[0] & ~MGEF_HIDE_IN_UI)
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
VANILLA_EFFECT_TEXT = {DRUGGED_EFFECT: "Your vision blurs for <dur> seconds.",
                       INVISIBILITY_EFFECT: "Invisible for <dur> seconds."}


def spell_description(effects):
    """What the spell does, in plain words, from the descriptions of its effects with the numbers filled in.
    (The spell's own DESC is what the magic menu and tooltips show, so it must say more than the name.)"""
    texts = {}
    for name, _, description, _, _ in MGEFS:
        texts[ids[name]] = PLAIN.get(name, description)
    texts.update(VANILLA_EFFECT_TEXT)
    skill_ids = {ids[skill_edid(name)] for name, _ in SKILLS} | {ids[wd_skill_edid(name)] for name, _ in SKILLS}
    rush_ids = {ids[rush_skill_edid(name, "Ab")] for name, _ in SKILLS}
    parts = []
    for item in effects:
        if item["mgef"] in rush_ids or item["mgef"] in skill_ids:
            continue  # hidden in the UI: the descriptor effect ("All Skills Boosted / Reduced") says it once
        text = texts.get(item["mgef"], "")
        text = (text.replace("<neg>", f"{abs(item['mag']):g}").replace("<pct>", f"{round(item['mag'] * 100):g}")
                .replace("<mag>", f"{item['mag']:g}")
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
    replace(subs, "DESC", esp.zstr(note_text(hold)))
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


def build_imad_overdose(edid):
    """Overdose warning: LowHealthImod with no blur (every blur track zeroed), the red tint's peak strength raised to
    OVERDOSE_TINT_ALPHA and a duration of OVERDOSE_IMOD_SECONDS instead of 1 s. Everything else is the vanilla record."""
    subs = template(format(IMOD_OVERDOSE_SOURCE, "08X"), "IMAD")
    replace(subs, "EDID", esp.zstr(edid))
    zero_blur(subs)
    data = dict(subs)["TNAM"]
    floats = list(struct.unpack(f"<{len(data) // 4}f", data))
    peak = max(range(4, len(floats), 5), key=lambda i: floats[i])  # the key with the strongest tint (alpha slot)
    floats[peak] = OVERDOSE_TINT_ALPHA
    replace(subs, "TNAM", struct.pack(f"<{len(floats)}f", *floats))
    dnam = dict(subs)["DNAM"]
    replace(subs, "DNAM", patch(dnam, 4, "<f", OVERDOSE_IMOD_SECONDS))
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
        "IMAD": [build_imad(IMAD_VELVET), build_imad_comedown(IMAD_COMEDOWN), build_imad_overdose(IMAD_OVERDOSE)],
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
    lines += ["", "    // Timed addiction / withdrawal spells: [drug or tier - 1][0 = the base spell, 1..N = its fixed-length variants V0..]."]
    lines.append(f"    inline constexpr int kTimedVariantCount = {TIMED_VARIANTS};")
    for label, names in (("kTimedAddicted", [lambda v, k=key: timed_addicted_edid(k, v) for key in DRUG_NAMES]),
                         ("kTimedWithdrawal", [lambda v, t=tier: timed_withdrawal_edid(t, v) for tier in (1, 2, 3)])):
        rows = []
        for namer in names:
            rows.append("{ " + ", ".join(f"0x{ids[namer(v)] & 0xFFFFFF:06X}" for v in [None] + list(range(TIMED_VARIANTS))) + " }")
        lines.append(f"    inline constexpr std::uint32_t {label}[3][{TIMED_VARIANTS + 1}] = {{ " + ", ".join(rows) + " };")
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
