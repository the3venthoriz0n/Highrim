#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RE
{
    class TESObjectREFR;
    class TESFaction;
    class TESWorldSpace;
}

namespace Highrim::Prison
{
    // One of the nine holds that have a note (Forms.h) and a vanilla crime faction.
    struct HoldDef
    {
        const char*   name;           // "Whiterun"
        const char*   capital;        // name of the hold capital's map marker (anchor fallback)
        std::uint32_t factionId;      // CrimeFaction<Hold> in Skyrim.esm (load order index 0, so a full id)
        std::uint32_t fallbackChest;  // PLCN of that faction in Skyrim.esm, 0 = vanilla has none
        std::uint32_t fallbackJail;   // JAIL of that faction in Skyrim.esm
        std::uint32_t noteLocalId;    // Forms::HighrimNote<Hold>
    };

    [[nodiscard]] const std::vector<HoldDef>& Holds();

    struct Resolved
    {
        const HoldDef*     def{ nullptr };
        RE::TESFaction*    faction{ nullptr };
        RE::TESObjectREFR* chest{ nullptr };
        RE::TESObjectREFR* jail{ nullptr };
        std::string        chestSource;  // "faction", "fallback" or "missing"
        std::string        jailSource;
    };

    // Looks up faction, chest and jail marker for one hold (game thread, after data loaded). Never throws.
    [[nodiscard]] Resolved Resolve(const HoldDef& def);

    struct Anchor
    {
        float              x{ 0 };
        float              y{ 0 };
        RE::TESWorldSpace* world{ nullptr };
        std::string        how;  // what the position was derived from, for the log
    };

    // The exterior position that stands for a reference: its own position if it is in an exterior cell,
    // otherwise the exterior side of the nearest door chain out of its interior. nullopt if unknown.
    [[nodiscard]] std::optional<Anchor> ExteriorAnchor(RE::TESObjectREFR* ref);

    struct Marker
    {
        RE::TESObjectREFR* ref;
        float              x, y;
        std::string        name;
        bool               visible;
    };

    // All map markers of a worldspace (from its persistent cell). Empty if none could be read.
    [[nodiscard]] std::vector<Marker> WorldMarkers(RE::TESWorldSpace* world);

    // Logs one info line per hold: faction, chest, jail marker, cell, anchor (or "missing").
    void Discover();
}
