"""Generates src/GameData.h from Skyrim.esm: the prison anchors, the Tamriel position of every child
worldspace and the Tamriel map markers (wake spots). Nothing here is read from the running game: the
game's cells are often not loaded when a blackout needs these numbers (the owner's log showed seven of
eight prisons without an anchor), so the values come from the master file instead.

Usage:
    python -I tools/extract_game_data.py <Skyrim.esm>            write src/GameData.h
    python -I tools/extract_game_data.py --check <Skyrim.esm>    exit 1 if src/GameData.h differs

What it computes (all coordinates are Tamriel space, 0x3C):
  * hold anchor: the position of the hold's JAIL marker when it lies in Tamriel; for the five holds whose
    marker lies in a city worldspace, the mean position of the Tamriel doors that lead into that city.
    The chest's interior is checked to lead (by its doors) into the same worldspace as the JAIL marker.
  * worlds: every worldspace whose parent is Tamriel, with the mean Tamriel position of the doors that lead
    into it (a door in another child worldspace counts through that worldspace), and the hold whose capital
    it belongs to, if any.
  * markers: every map marker (XMRK) in worldspace Tamriel that is not deleted or initially disabled.
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(ROOT, "src", "GameData.h")
TAMRIEL = 0x3C

# name, CrimeFaction, confiscation chest REFR (0 = none in vanilla), JAIL marker REFR (same ids as src/Prison.cpp)
HOLDS = [
    ("Whiterun", 0x000267EA, 0x000267E8, 0x000267E5),
    ("Eastmarch", 0x000267E3, 0x0003EF10, 0x0003EF18),
    ("Rift", 0x0002816B, 0x000A8F33, 0x0003EF58),
    ("Haafingar", 0x00029DB0, 0x0003EEFF, 0x0003EF19),
    ("Hjaalmarch", 0x0002816D, 0x0003EF09, 0x0003EF1A),
    ("Reach", 0x0002816C, 0x0003EF03, 0x0003EF57),
    ("Falkreath", 0x00028170, 0x000EF437, 0x0003EF17),
    ("Pale", 0x0002816E, 0x0003EF12, 0x0003EF1B),
    ("Winterhold", 0x0002816F, 0, 0x0003EF59),
]

DELETED = 0x20
DISABLED = 0x800
COMPRESSED = 0x40000


def subrecords(body):
    out, pos, ext = [], 0, None
    while pos < len(body):
        tag = body[pos:pos + 4].decode("ascii")
        size = struct.unpack_from("<H", body, pos + 4)[0]
        pos += 6
        if tag == "XXXX":
            ext = struct.unpack_from("<I", body, pos)[0]
            pos += 4
            continue
        if ext is not None:
            size, ext = ext, None
        out.append((tag, body[pos:pos + size]))
        pos += size
    return out


def read_esm(path):
    """Walks the whole file. Returns (worlds, cells, refs).
    worlds: {id: parent id or 0}; cells: {id: (world id or 0 for interiors, is_interior)};
    refs: {id: dict(world, cell, flags, pos, xtel, marker)} for every REFR that has a position."""
    with open(path, "rb") as handle:
        data = handle.read()
    worlds, cells, refs = {}, {}, {}

    def walk(pos, end, world, cell):
        while pos < end:
            sig = data[pos:pos + 4]
            size = struct.unpack_from("<I", data, pos + 4)[0]
            if sig == b"GRUP":
                label = struct.unpack_from("<I", data, pos + 8)[0]
                gtype = struct.unpack_from("<I", data, pos + 12)[0]
                w, c = world, cell
                if gtype == 1:
                    w = label
                if gtype in (6, 8, 9, 10):
                    c = label
                walk(pos + 24, pos + size, w, c)
                pos += size
                continue
            flags, formid = struct.unpack_from("<II", data, pos + 8)
            if sig in (b"WRLD", b"CELL", b"REFR"):
                body = data[pos + 24:pos + 24 + size]
                if flags & COMPRESSED:
                    body = zlib.decompress(body[4:])
                subs = dict()
                for tag, payload in subrecords(body):
                    subs.setdefault(tag, payload)
                if sig == b"WRLD":
                    wnam = subs.get("WNAM")
                    worlds[formid] = struct.unpack("<I", wnam[:4])[0] if wnam else 0
                elif sig == b"CELL":
                    cells[formid] = (world, bool(subs["DATA"][0] & 1) if "DATA" in subs else False)
                elif "DATA" in subs and len(subs["DATA"]) >= 12:
                    x, y, z = struct.unpack_from("<fff", subs["DATA"])
                    xtel = struct.unpack_from("<I", subs["XTEL"])[0] if "XTEL" in subs else 0
                    refs[formid] = dict(world=world, cell=cell, flags=flags, pos=(x, y), xtel=xtel,
                                        marker="XMRK" in subs)
            pos += 24 + size

    header = struct.unpack_from("<I", data, 4)[0]
    walk(24 + header, len(data), 0, 0)
    return worlds, cells, refs


def cell_world(cells, cell_id):
    """Worldspace of a cell, 0 for interiors."""
    world, interior = cells.get(cell_id, (0, True))
    return 0 if interior else world


def build(path):
    worlds, cells, refs = read_esm(path)
    children = sorted(w for w, parent in worlds.items() if parent == TAMRIEL)

    # entrances[w]: (x, y, source world) of every exterior door in another worldspace that leads into w.
    entrances = {}
    for rid, r in refs.items():
        if not r["xtel"] or r["flags"] & (DELETED | DISABLED):
            continue
        dest = refs.get(r["xtel"])
        if not dest:
            continue
        src_world, dst_world = r["world"], cell_world(cells, dest["cell"])
        if src_world and dst_world and src_world != dst_world and cells.get(r["cell"], (0, True))[1] is False:
            entrances.setdefault(dst_world, []).append((r["pos"][0], r["pos"][1], src_world))

    city_of = {}  # world -> hold index it is the capital of (by the prison marker)
    for index, (name, faction, chest, jail) in enumerate(HOLDS):
        jw = refs[jail]["world"]
        if jw != TAMRIEL:
            city_of[jw] = index

    def resolve(world, seen=()):
        """(x, y) in Tamriel space for a child worldspace, or None."""
        points = []
        for x, y, src in entrances.get(world, []):
            if src == TAMRIEL:
                points.append((x, y))
            elif src not in seen and src in worlds:
                inner = resolve(src, seen + (world,))
                if inner:
                    points.append(inner)
        points = sorted(set(points))
        if not points:
            return None
        return (sum(p[0] for p in points) / len(points), sum(p[1] for p in points) / len(points))

    def owner(world, seen=()):
        if world in city_of:
            return city_of[world]
        for _x, _y, src in entrances.get(world, []):
            if src != TAMRIEL and src not in seen:
                found = owner(src, seen + (world,))
                if found is not None:
                    return found
        return None

    by_cell = {}
    for r in refs.values():
        if r["xtel"] and not r["flags"] & DELETED:
            by_cell.setdefault(r["cell"], []).append(r)

    def interior_exit_world(cell_id):
        """Worldspace reached by the shortest door chain out of an interior cell, or None."""
        queue, seen = [cell_id], {cell_id}
        while queue:
            current = queue.pop(0)
            for door in by_cell.get(current, []):
                dest = refs.get(door["xtel"])
                if not dest:
                    continue
                world = cell_world(cells, dest["cell"])
                if world:
                    return world
                if dest["cell"] not in seen:
                    seen.add(dest["cell"])
                    queue.append(dest["cell"])
        return None

    anchors = []
    for index, (name, faction, chest, jail) in enumerate(HOLDS):
        jw = refs[jail]["world"]
        if jw == TAMRIEL:
            x, y = refs[jail]["pos"]
            how = "jail marker in Tamriel"
        else:
            point = resolve(jw)
            if point is None:
                raise SystemExit(f"{name}: no Tamriel door leads into its city worldspace {jw:08X}")
            x, y = point
            how = f"mean of the Tamriel doors into city worldspace {jw:08X}"
        if chest:
            exit_world = interior_exit_world(refs[chest]["cell"])
            if exit_world != jw:
                raise SystemExit(f"{name}: the chest's interior leads into worldspace {exit_world} but the jail marker is in {jw:08X}")
        anchors.append((name, faction, round(x), round(y), jw if jw != TAMRIEL else 0, how))

    world_rows = []
    for w in children:
        point = resolve(w)
        if point:
            world_rows.append((w, owner(w), round(point[0]), round(point[1])))

    markers = []
    for rid, r in refs.items():
        if r["marker"] and r["world"] == TAMRIEL and not r["flags"] & (DELETED | DISABLED):
            markers.append((rid, round(r["pos"][0]), round(r["pos"][1])))
    markers.sort()
    return anchors, world_rows, markers


def render(anchors, world_rows, markers):
    out = []
    w = out.append
    w("// Generated by tools/extract_game_data.py from Skyrim.esm. Do not edit; rerun the tool (--check verifies).")
    w("// All positions are Tamriel space (worldspace 0000003C).")
    w("#pragma once")
    w("")
    w("#include <cstdint>")
    w("")
    w("namespace Highrim::GameData")
    w("{")
    w("    // Where a hold's prison is, in Tamriel space. cityWorld: the child worldspace the jail marker lies in (0 = Tamriel).")
    w("    struct HoldAnchor")
    w("    {")
    w("        std::uint32_t factionId;")
    w("        float         x, y;")
    w("        std::uint32_t cityWorld;")
    w("    };")
    w("    inline constexpr HoldAnchor kHoldAnchors[] = {")
    for name, faction, x, y, city, how in anchors:
        w(f"        {{ 0x{faction:08X}, {x}.0f, {y}.0f, 0x{city:08X} }},  // {name}: {how}")
    w("    };")
    w("")
    w("    // Child worldspaces of Tamriel: where their door(s) stand in Tamriel, and the hold (CrimeFaction id) whose capital")
    w("    // they belong to, 0 if none.")
    w("    struct WorldEntry")
    w("    {")
    w("        std::uint32_t world;")
    w("        std::uint32_t holdFaction;")
    w("        float         x, y;")
    w("    };")
    w("    inline constexpr WorldEntry kWorlds[] = {")
    faction_of = {i: a[1] for i, a in enumerate(anchors)}
    for world, hold, x, y in world_rows:
        faction = faction_of[hold] if hold is not None else 0
        w(f"        {{ 0x{world:08X}, 0x{faction:08X}, {x}.0f, {y}.0f }},")
    w("    };")
    w("")
    w("    // Every map marker reference of Tamriel (candidate wake spots), by form id.")
    w("    struct Marker")
    w("    {")
    w("        std::uint32_t id;")
    w("        float         x, y;")
    w("    };")
    w("    inline constexpr Marker kTamrielMarkers[] = {")
    for rid, x, y in markers:
        w(f"        {{ 0x{rid:08X}, {x}.0f, {y}.0f }},")
    w("    };")
    w("}")
    return "\n".join(out) + "\n"


def main(argv):
    check = "--check" in argv
    args = [a for a in argv if a != "--check"]
    if len(args) != 1:
        sys.exit(__doc__)
    text = render(*build(args[0]))
    if check:
        with open(OUTPUT, "r", encoding="utf-8", newline="") as handle:
            current = handle.read()
        if current != text:
            print("src/GameData.h is out of date; rerun tools/extract_game_data.py")
            return 1
        print("src/GameData.h matches Skyrim.esm")
        return 0
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print(f"Wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
