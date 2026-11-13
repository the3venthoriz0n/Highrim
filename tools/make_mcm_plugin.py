"""Writes the tiny MCM registration plugin that MCM Helper needs to list a mod's settings menu.

MCM Helper only shows a mod if (a) a plugin named like the config folder is loaded and (b) that plugin
has a start-game-enabled quest with the MCM_ConfigBase script (MCM Helper's own script, attached as is)
and SkyUI's SKI_PlayerLoadGameAlias on the player alias. The record layout follows what the Creation Kit
writes for such a quest. No gameplay records; light (ESL) flagged.

Usage: python -I tools/make_mcm_plugin.py <ModName> <output.esp>
"""
import struct
import sys


def sub(tag: bytes, data: bytes) -> bytes:
    return tag + struct.pack("<H", len(data)) + data


def zstr(text: str) -> bytes:
    return text.encode("ascii") + b"\0"


def wstring(text: str) -> bytes:
    raw = text.encode("ascii")
    return struct.pack("<H", len(raw)) + raw


def build(mod_name: str) -> bytes:
    quest_id = 0x01000D62  # master index 1 (Skyrim.esm), light-plugin local id 0xD62
    editor_id = f"{mod_name}_MCM"

    # Papyrus data: one script (MCM_ConfigBase, no property overrides), then the quest's alias block
    # with SKI_PlayerLoadGameAlias on the player alias. The alias block is copied byte for byte from a
    # working MCM Helper quest (Better Third Person Selection); it holds this quest's own FormID.
    vmad = struct.pack("<HHH", 5, 2, 1)
    vmad += wstring("MCM_ConfigBase") + struct.pack("<BH", 0, 0)
    vmad += bytes.fromhex("0200000000010000000000") + struct.pack("<I", quest_id)
    vmad += bytes.fromhex("0500020001001700") + b"SKI_PlayerLoadGameAlias" + bytes.fromhex("000000")

    quest = b""
    quest += sub(b"EDID", zstr(editor_id))
    quest += sub(b"VMAD", vmad)
    quest += sub(b"FULL", zstr(editor_id))
    quest += sub(b"DNAM", bytes.fromhex("010100b000000000" "00000000"))  # start game enabled
    quest += sub(b"NEXT", b"")
    quest += sub(b"ANAM", struct.pack("<I", 1))
    quest += sub(b"ALST", struct.pack("<I", 0))
    quest += sub(b"ALID", zstr("PlayerAlias"))
    quest += sub(b"FNAM", struct.pack("<I", 0))
    quest += sub(b"ALFR", struct.pack("<I", 0x14))
    quest += sub(b"VTCK", struct.pack("<I", 0))
    quest += sub(b"ALED", b"")

    stamp = 11344
    qust_record = b"QUST" + struct.pack("<IIIIHH", len(quest), 0, quest_id, stamp, 44, 0) + quest
    group = b"GRUP" + struct.pack("<I", 24 + len(qust_record)) + b"QUST" + struct.pack("<III", 0, stamp, 0) + qust_record

    header = b""
    header += sub(b"HEDR", struct.pack("<fII", 1.7, 2, 0x0D63))
    header += sub(b"CNAM", zstr("the3venthoriz0n"))
    header += sub(b"MAST", zstr("Skyrim.esm"))
    header += sub(b"DATA", bytes(8))
    header += sub(b"INTV", struct.pack("<I", 1))
    tes4 = b"TES4" + struct.pack("<IIIIHH", len(header), 0x200, 0, 0, 44, 0) + header  # 0x200 = light plugin

    return tes4 + group


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = build(sys.argv[1])
    with open(sys.argv[2], "wb") as handle:
        handle.write(data)
    print(f"Wrote {sys.argv[2]} ({len(data)} bytes)")
