"""Builds THIRD_PARTY_NOTICES.txt from the real licence files of everything linked into the plugin.

Usage: python -I tools/make_notices.py [--share <dir>] [--check]

The vcpkg ports come from this repo's vcpkg.json; each port's `copyright` file is read from the vcpkg
`share` folder of the x64-windows-static-md triplet (the one the plugin is built with). The folder is
searched in: --share, $VCPKG_INSTALLED_SHARE, out/build-release/vcpkg_installed/x64-windows-static-md/share,
C:/dev/vcpkg/installed/x64-windows-static-md/share. A used port without a copyright file is an error.
The licence files of the CommonLibSSE-NG submodule (and of the code it vendors) are added from lib/.
--check only compares with the committed file and exits 1 when it differs.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "THIRD_PARTY_NOTICES.txt")
TRIPLET = "x64-windows-static-md"

# vcpkg port -> (display name, project URL)
PORTS = {
    "directxmath": ("DirectXMath", "https://github.com/microsoft/DirectXMath"),
    "directxtk": ("DirectX Tool Kit (DirectXTK)", "https://github.com/microsoft/DirectXTK"),
    "fmt": ("{fmt}", "https://github.com/fmtlib/fmt"),
    "nlohmann-json": ("JSON for Modern C++ (nlohmann/json)", "https://github.com/nlohmann/json"),
    "rapidcsv": ("rapidcsv", "https://github.com/d99kris/rapidcsv"),
    "simpleini": ("SimpleIni", "https://github.com/brofield/simpleini"),
    "spdlog": ("spdlog", "https://github.com/gabime/spdlog"),
    "toml11": ("toml11", "https://github.com/ToruNiina/toml11"),
    "xbyak": ("Xbyak", "https://github.com/herumi/xbyak"),
}

EXTRA_NOTES = []

# (title, URL, path relative to the repo root, required)
LIB = "lib/commonlibsse-ng"
SUBMODULE_FILES = [
    ("CommonLibSSE-NG (alandtse fork) - GNU GPL v3", "https://github.com/alandtse/CommonLibSSE-NG", LIB + "/COPYING.txt", True),
    ("CommonLibSSE-NG - Modding Exception and Linking Exception", "https://github.com/alandtse/CommonLibSSE-NG", LIB + "/EXCEPTIONS.md", True),
    ("CommonLibSSE (original code by Ryan-rsm-McKenzie) - MIT License",
     "https://github.com/Ryan-rsm-McKenzie/CommonLibSSE", LIB + "/licenses/LICENSE-MIT.txt", True),
    ("hde64 instruction-length decoder (from MinHook v1.3.4, fetched at build time by CommonLibSSE-NG)",
     "https://github.com/TsudaKageyu/minhook", LIB + "/licenses/LICENSE-hde64.txt", False),
    ("OpenVR headers and library (Valve), used for the VR support in CommonLibSSE-NG",
     "https://github.com/ValveSoftware/openvr", LIB + "/extern/openvr/LICENSE", False),
]


def find_share():
    cands = []
    if "--share" in sys.argv:
        cands.append(sys.argv[sys.argv.index("--share") + 1])
    if os.environ.get("VCPKG_INSTALLED_SHARE"):
        cands.append(os.environ["VCPKG_INSTALLED_SHARE"])
    cands.append(os.path.join(ROOT, "out", "build-release", "vcpkg_installed", TRIPLET, "share"))
    cands.append("C:/dev/vcpkg/installed/" + TRIPLET + "/share")
    for c in cands:
        if os.path.isdir(c):
            return c
    sys.exit("No vcpkg share folder found. Build once, or pass --share <dir>. Tried: " + ", ".join(cands))


def read(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", errors="replace").replace("\r\n", "\n").strip("\n")


def main():
    with open(os.path.join(ROOT, "vcpkg.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    used = []
    for dep in manifest["dependencies"]:
        used.append(dep if isinstance(dep, str) else dep["name"])
    share = find_share()
    sections = []
    for port in used:
        if port not in PORTS:
            sys.exit("vcpkg.json uses '%s', which has no entry in PORTS in tools/make_notices.py" % port)
        name, url = PORTS[port]
        folder = None
        for entry in os.listdir(share):  # vcpkg share folders are not always lower case (SimpleIni)
            if entry.lower() == port.lower():
                folder = os.path.join(share, entry)
        path = os.path.join(folder, "copyright") if folder else None
        if not path or not os.path.isfile(path):
            sys.exit("No copyright file for vcpkg port '%s' under %s" % (port, share))
        sections.append((name, url, read(path)))
    for title, url, rel, required in SUBMODULE_FILES:
        path = os.path.join(ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(path):
            if required:
                sys.exit("Missing required licence file: " + rel)
            continue
        sections.append((title, url, read(path)))

    for note in EXTRA_NOTES:
        sections.append(("Other code in this repository", "", note))
    bar = "=" * 78
    parts = [
        "THIRD-PARTY NOTICES\n\n"
        "This mod is licensed GPL-3.0-or-later (see COPYING). It statically links the libraries below.\n"
        "Their licence texts are reproduced unmodified from the vcpkg packages and the CommonLibSSE-NG\n"
        "source that were used to build it. Generated by tools/make_notices.py.\n"
    ]
    for name, url, text in sections:
        parts.append("%s\n%s\n%s\n%s\n\n%s\n" % (bar, name, url, bar, text))
    out = "\n".join(parts)
    if "--check" in sys.argv:
        old = open(OUT, encoding="utf-8", newline="").read() if os.path.isfile(OUT) else ""
        if old.replace("\r\n", "\n") != out:
            sys.exit("THIRD_PARTY_NOTICES.txt is out of date; run python -I tools/make_notices.py")
        print("THIRD_PARTY_NOTICES.txt is up to date")
        return
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)
    print("Wrote %s (%d sections: %s)" % (OUT, len(sections), ", ".join(s[0] for s in sections)))


if __name__ == "__main__":
    main()
