"""Copies the vanilla records that tools/make_esp.py clones out of Skyrim.esm into
tools/vanilla_templates.json, so the generator runs without the game installed.

Usage: python -I tools/extract_templates.py <path to Skyrim.esm>
The output is committed; rerun only to add a template.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esp  # noqa: E402

TEMPLATES = {
    0x0003EAF9: "AlchFortifyStamina (potion MGEF, peak value modifier, fire and forget)",
    0x0003EB08: "AlchFortifyStaminaRate (potion MGEF)",
    0x000E3CBA: "dunSleepingTreeSlowAlch (sap MGEF: slows movement; used by an ALCH)",
    0x00073F2C: "AlchDamageStaminaRate (potion MGEF)",
    0x000C1E8D: "AbDamageStaminaRate (constant-effect MGEF)",
    0x00091F77: "AbFortifyStamina (constant-effect MGEF)",
    0x0010DE1A: "VampireAbDamageHealRate (constant-effect MGEF)",
    0x00057A7A: "Skooma (ALCH)",
    0x000E1A9F: "dunSleepingTreeCampNote01 (BOOK note)",
    0x0010FB30: "VampirePoisonResist (ability SPEL)",
    0x0005AD5C: "Oakflesh (fire and forget self SPEL, for the SPIT layout)",
    0x000F431F: "RecipeFoodElsweyrFondue (cooking pot COBJ)",
    0x0008CDEC: "VendorItemPotion (KYWD)",
    0x001019D8: "DetectLifeEnemyExteriorSelfFF (Detect Life MGEF)",
    0x001019D9: "DetectLifeEnemyInteriorSelfFF (Detect Life MGEF)",
    0x001019DA: "DetectLifeFriendExteriorSelfFF (Detect Life MGEF)",
    0x001019DB: "DetectLifeFriendInteriorSelfFF (Detect Life MGEF)",
}


def main(path):
    found = esp.read_records(path, set(TEMPLATES))
    out = {}
    for formid in sorted(found):
        rtype, subs = found[formid]
        edid = dict(subs)["EDID"][:-1].decode("ascii")
        out[f"{formid:08X}"] = {"type": rtype, "edid": edid, "note": TEMPLATES[formid],
                                "subs": [[tag, data.hex()] for tag, data in subs]}
    target = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vanilla_templates.json")
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(out, handle, indent=1, sort_keys=True)
        handle.write("\n")
    print(f"Wrote {target} ({len(out)} templates)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
