"""Small Skyrim SE plugin writer and reader (record header 24 bytes, no compression on write).

Reading handles compressed records (flag 0x00040000: 4-byte size, then zlib) because Skyrim.esm has them;
it is used by tools/extract_templates.py to copy vanilla records. Writing is used by tools/make_esp.py.
"""
import struct
import zlib

STAMP = 11344        # the datestamp/version-control word the original MCM plugin writer used
FORM_VERSION = 44    # SSE record version
COMPRESSED = 0x00040000


# --- writing ---------------------------------------------------------------------------------------
def sub(tag: bytes, data: bytes) -> bytes:
    if len(data) > 0xFFFF:
        raise ValueError(f"subrecord {tag!r} too large")
    return tag + struct.pack("<H", len(data)) + data


def zstr(text: str) -> bytes:
    return text.encode("ascii") + b"\0"


def wstring(text: str) -> bytes:
    raw = text.encode("ascii")
    return struct.pack("<H", len(raw)) + raw


def join_subs(subs) -> bytes:
    """subs: list of (tag: str, data: bytes) -> concatenated subrecords."""
    return b"".join(sub(tag.encode("ascii"), data) for tag, data in subs)


def record(rtype: bytes, formid: int, body: bytes, flags: int = 0) -> bytes:
    return rtype + struct.pack("<IIIIHH", len(body), flags, formid, STAMP, FORM_VERSION, 0) + body


def group(label: bytes, records: list) -> bytes:
    body = b"".join(records)
    return b"GRUP" + struct.pack("<I", 24 + len(body)) + label + struct.pack("<III", 0, STAMP, 0) + body


def tes4(masters, author: str, record_count: int, next_object_id: int, flags: int) -> bytes:
    body = sub(b"HEDR", struct.pack("<fII", 1.7, record_count, next_object_id))
    body += sub(b"CNAM", zstr(author))
    for master in masters:
        body += sub(b"MAST", zstr(master)) + sub(b"DATA", bytes(8))
    body += sub(b"INTV", struct.pack("<I", 1))
    return b"TES4" + struct.pack("<IIIIHH", len(body), flags, 0, 0, FORM_VERSION, 0) + body


# --- reading (vanilla templates) ---------------------------------------------------------------------
def _subrecords(body: bytes):
    subs, pos, extended = [], 0, None
    while pos < len(body):
        tag = body[pos:pos + 4].decode("ascii")
        size = struct.unpack_from("<H", body, pos + 4)[0]
        pos += 6
        if tag == "XXXX":
            extended = struct.unpack_from("<I", body, pos)[0]
            pos += 4
            continue
        if extended is not None:
            size, extended = extended, None
        subs.append((tag, body[pos:pos + size]))
        pos += size
    return subs


def read_records(path, wanted):
    """Return {formid: (type, subs)} for the records whose form IDs are in `wanted` (a set of ints)."""
    data = open(path, "rb").read()
    found = {}
    pending = set(wanted)
    size = struct.unpack_from("<I", data, 4)[0]
    pos = 24 + size
    while pos < len(data) and pending:
        sig = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        if sig == b"GRUP":
            pos += 24  # descend: records follow the group header directly
            continue
        flags, formid = struct.unpack_from("<II", data, pos + 8)
        if formid in pending:
            body = data[pos + 24:pos + 24 + size]
            if flags & COMPRESSED:
                expected = struct.unpack_from("<I", body)[0]
                body = zlib.decompress(body[4:])
                if len(body) != expected:
                    raise ValueError(f"bad decompressed size for {formid:08X}")
            found[formid] = (sig.decode("ascii"), _subrecords(body))
            pending.discard(formid)
        pos += 24 + size
    if pending:
        raise KeyError("not found: " + ", ".join(f"{f:08X}" for f in sorted(pending)))
    return found
