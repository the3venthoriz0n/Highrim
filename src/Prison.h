#pragma once

#include "Core.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RE
{
    class PlayerCharacter;
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

    // A hold's prison position in Tamriel space and its capital worldspace(s), from the generated GameData.h
    // (read out of Skyrim.esm, so it does not depend on which cells are loaded). nullopt if the hold is not in it.
    [[nodiscard]] std::optional<Core::Point> TamrielAnchor(const HoldDef& def);

    // Core::HoldInfo (chest present, Tamriel anchor) for every hold, in Holds() order, and the capital worldspaces.
    [[nodiscard]] std::vector<Core::HoldInfo>  HoldInfos(const std::vector<Resolved>& resolved);
    [[nodiscard]] std::vector<Core::WorldHold> WorldHolds();

    // Where the player is: the worldspace id (0 = unknown) and the matching position in Tamriel space, if any.
    struct PlayerPlace
    {
        std::uint32_t                world{ 0 };
        std::optional<Core::Point>   tamriel;
        std::string                  how;
    };
    [[nodiscard]] PlayerPlace LocatePlayer(RE::PlayerCharacter* player);

    // A candidate wake spot: a Tamriel map marker reference and its position.
    struct WakeMarker
    {
        RE::TESObjectREFR* ref;
        std::uint32_t      id;
        float              x, y;
    };
    struct WakeMarkers
    {
        std::vector<WakeMarker> usable;
        int                     tableTotal{ 0 };
        int                     tableFound{ 0 };     // table ids that resolved to a reference
        int                     liveFound{ 0 };      // markers read from Tamriel's persistent cell right now
        int                     liveExtra{ 0 };      // of those, not in the table (other mods)
        int                     noCell{ 0 };         // references skipped: no cell to teleport into
        int                     moved{ 0 };          // table markers whose live position differs from the table
    };
    // Every map marker of Tamriel: the table from Skyrim.esm plus whatever Tamriel's persistent cell lists now.
    [[nodiscard]] WakeMarkers CollectWakeMarkers();

    // Logs one info line per hold: faction, chest, jail marker, cell, anchor (or "missing").
    void Discover();
}
