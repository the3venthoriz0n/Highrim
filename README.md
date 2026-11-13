# Highrim

A Skyrim SE SKSE mod (version 0.1.0, work in progress): craftable cooking-pot drugs, addiction with withdrawal, and an overdose blackout that moves your gear to the hold's prison chest and wakes you elsewhere.

**Status: scaffold only.** This build contains the plugin skeleton, the MCM settings menu and the settings code. The drugs, addiction and blackout are not implemented yet, and nothing has been verified in game.

Built with C++23, CommonLibSSE-NG v15.7.3 (git submodule) and vcpkg. Needs SKSE, Address Library and MCM Helper.

## Build

    git submodule update --init --recursive
    build_release.bat      (from a VS x64 command prompt)
    make_archive.bat       (writes dist\Highrim-0.1.0.zip)

Tests: `ctest --test-dir out\build-release -C Release --output-on-failure`.

Design: `docs/superpowers/specs/2026-11-13-highrim-design.md`.

## License

CommonLibSSE-NG is GPL-3.0-or-later and is linked into the plugin, so Highrim is GPL too (see COPYING).
