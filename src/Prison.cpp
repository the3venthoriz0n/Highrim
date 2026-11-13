#include "PCH.h"

#include "Prison.h"

#include "Forms.h"
#include "GameData.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

namespace Highrim::Prison
{
    const std::vector<HoldDef>& Holds()
    {
        // Faction, PLCN (player inventory container) and JAIL ids were read from Skyrim.esm with a parser;
        // they are only used when the faction object itself exposes no chest/marker. Winterhold has no PLCN in
        // vanilla: it has a jail marker but no confiscation chest.
        static const std::vector<HoldDef> holds = {
            { "Whiterun", 0x000267EA, 0x000267E8, 0x000267E5, Forms::HighrimNoteWhiterun },
            { "Eastmarch", 0x000267E3, 0x0003EF10, 0x0003EF18, Forms::HighrimNoteEastmarch },
            { "Rift", 0x0002816B, 0x000A8F33, 0x0003EF58, Forms::HighrimNoteRift },
            { "Haafingar", 0x00029DB0, 0x0003EEFF, 0x0003EF19, Forms::HighrimNoteHaafingar },
            { "Hjaalmarch", 0x0002816D, 0x0003EF09, 0x0003EF1A, Forms::HighrimNoteHjaalmarch },
            { "Reach", 0x0002816C, 0x0003EF03, 0x0003EF57, Forms::HighrimNoteReach },
            { "Falkreath", 0x00028170, 0x000EF437, 0x0003EF17, Forms::HighrimNoteFalkreath },
            { "Pale", 0x0002816E, 0x0003EF12, 0x0003EF1B, Forms::HighrimNotePale },
            { "Winterhold", 0x0002816F, 0,          0x0003EF59, Forms::HighrimNoteWinterhold },
        };
        return holds;
    }

    namespace
    {
        RE::TESWorldSpace* WorldOf(RE::TESObjectCELL* cell)
        {
            return cell ? cell->GetRuntimeData().worldSpace : nullptr;
        }

        // The cell a reference is in; for a reference whose cell is not attached right now, the cell it is saved in.
        RE::TESObjectCELL* CellOf(RE::TESObjectREFR* ref)
        {
            if (!ref) {
                return nullptr;
            }
            auto* cell = ref->GetParentCell();
            return cell ? cell : ref->GetSaveParentCell();
        }

        const char* CellName(RE::TESObjectCELL* cell)
        {
            if (!cell) {
                return "<no cell>";
            }
            const char* n = cell->GetName();
            return (n && *n) ? n : "<unnamed>";
        }

        std::string DescribeRef(RE::TESObjectREFR* ref)
        {
            if (!ref) {
                return "null";
            }
            auto*      cell = CellOf(ref);
            const auto pos  = ref->GetPosition();
            return std::format("{:08X} cell={} ({:08X}, {}) pos=({:.0f},{:.0f},{:.0f})", ref->GetFormID(),
                CellName(cell), cell ? cell->GetFormID() : 0u,
                cell ? (cell->IsInteriorCell() ? "interior" : "exterior") : "unknown", pos.x, pos.y, pos.z);
        }
    }

    Resolved Resolve(const HoldDef& def)
    {
        Resolved r;
        r.def         = &def;
        r.chestSource = "missing";
        r.jailSource  = "missing";
        try {
            r.faction = RE::TESForm::LookupByID<RE::TESFaction>(def.factionId);
            if (r.faction) {
                r.chest = r.faction->crimeData.factionPlayerInventoryContainer;
                r.jail  = r.faction->crimeData.factionJailMarker;
                if (r.chest) {
                    r.chestSource = "faction";
                }
                if (r.jail) {
                    r.jailSource = "faction";
                }
            }
            if (!r.chest && def.fallbackChest) {
                if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(def.fallbackChest)) {
                    r.chest       = ref;
                    r.chestSource = "fallback";
                }
            }
            if (!r.jail && def.fallbackJail) {
                if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(def.fallbackJail)) {
                    r.jail       = ref;
                    r.jailSource = "fallback";
                }
            }
        } catch (...) {
            spdlog::error("Prison: exception resolving {}", def.name);
        }
        return r;
    }

    std::optional<Core::Point> TamrielAnchor(const HoldDef& def)
    {
        for (const auto& a : GameData::kHoldAnchors) {
            if (a.factionId == def.factionId) {
                return Core::Point{ a.x, a.y };
            }
        }
        return std::nullopt;
    }

    std::vector<Core::HoldInfo> HoldInfos(const std::vector<Resolved>& resolved)
    {
        std::vector<Core::HoldInfo> out;
        for (const auto& r : resolved) {
            const auto anchor = TamrielAnchor(*r.def);
            out.push_back({ r.chest != nullptr && anchor.has_value(), anchor.value_or(Core::Point{ 0.f, 0.f }) });
        }
        return out;
    }

    std::vector<Core::WorldHold> WorldHolds()
    {
        std::vector<Core::WorldHold> out;
        const auto&                  holds = Holds();
        auto indexOf = [&](std::uint32_t faction) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < holds.size(); ++i) {
                if (holds[i].factionId == faction) {
                    return i;
                }
            }
            return std::nullopt;
        };
        for (const auto& a : GameData::kHoldAnchors) {
            if (a.cityWorld != 0) {
                if (auto i = indexOf(a.factionId)) {
                    out.push_back({ a.cityWorld, *i });
                }
            }
        }
        for (const auto& w : GameData::kWorlds) {
            if (w.holdFaction != 0) {
                if (auto i = indexOf(w.holdFaction)) {
                    out.push_back({ w.world, *i });
                }
            }
        }
        return out;
    }

    PlayerPlace LocatePlayer(RE::PlayerCharacter* player)
    {
        constexpr std::uint32_t kTamriel = 0x0000003C;
        PlayerPlace             place;
        if (!player) {
            place.how = "no player";
            return place;
        }
        float                  x = 0.f, y = 0.f;
        RE::TESWorldSpace*     world = nullptr;
        bool                   known = false;
        if (auto* cell = CellOf(player); cell && cell->IsExteriorCell()) {
            const auto pos = player->GetPosition();
            x = pos.x;
            y = pos.y;
            world = WorldOf(cell);
            known = world != nullptr;
            place.how = "player's own exterior cell";
        } else if (auto a = ExteriorAnchor(player)) {
            x     = a->x;
            y     = a->y;
            world = a->world;
            known = world != nullptr;
            place.how = "player anchor: " + a->how;
        }
        if (!known) {
            const auto& rt  = player->GetPlayerRuntimeData();
            const auto  ext = rt.exteriorPosition;
            if (place.how.empty()) {
                x = ext.x;
                y = ext.y;
                world = rt.cachedWorldSpace;
                place.how = "player last exteriorPosition (cachedWorldSpace)";
            } else {
                place.how += " (worldspace unknown)";
            }
        }
        place.world = world ? world->GetFormID() : 0u;
        if (place.world == 0 || place.world == kTamriel) {
            place.tamriel = Core::Point{ x, y };
            if (place.world == 0) {
                place.how += ", worldspace unknown so Tamriel assumed";
            }
            return place;
        }
        for (const auto& w : GameData::kWorlds) {
            if (w.world == place.world) {
                place.tamriel = Core::Point{ w.x, w.y };
                place.how += std::format(", worldspace {:08X} stands at Tamriel ({:.0f},{:.0f}) (own position was ({:.0f},{:.0f}))", place.world,
                    w.x, w.y, x, y);
                return place;
            }
        }
        place.tamriel = Core::Point{ x, y };
        place.how += std::format(", worldspace {:08X} is not in the Tamriel table so its own position is used as is", place.world);
        return place;
    }

    WakeMarkers CollectWakeMarkers()
    {
        WakeMarkers out;
        std::set<std::uint32_t> seen;
        auto add = [&](RE::TESObjectREFR* ref, std::uint32_t id) {
            if (!CellOf(ref)) {
                ++out.noCell;
                return;
            }
            const auto pos = ref->GetPosition();
            out.usable.push_back({ ref, id, pos.x, pos.y });
        };
        try {
            out.tableTotal = static_cast<int>(std::size(GameData::kTamrielMarkers));
            for (const auto& m : GameData::kTamrielMarkers) {
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(m.id);
                if (!ref) {
                    continue;
                }
                ++out.tableFound;
                seen.insert(m.id);
                const auto pos = ref->GetPosition();
                if (std::fabs(pos.x - m.x) > 2.0f || std::fabs(pos.y - m.y) > 2.0f) {
                    ++out.moved;
                }
                add(ref, m.id);
            }
            auto* tamriel = RE::TESForm::LookupByID<RE::TESWorldSpace>(0x0000003C);
            if (tamriel && tamriel->persistentCell) {
                tamriel->persistentCell->ForEachReference([&](RE::TESObjectREFR* ref) {
                    if (ref && ref->extraList.GetByType<RE::ExtraMapMarker>()) {
                        ++out.liveFound;
                        if (seen.insert(ref->GetFormID()).second) {
                            ++out.liveExtra;
                            add(ref, ref->GetFormID());
                        }
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
        } catch (...) {
            spdlog::error("Prison: exception collecting wake markers");
        }
        return out;
    }

    std::optional<Anchor> ExteriorAnchor(RE::TESObjectREFR* ref)
    {
        try {
            if (!ref) {
                return std::nullopt;
            }
            auto* cell = CellOf(ref);
            if (!cell) {
                return std::nullopt;
            }
            if (cell->IsExteriorCell()) {
                const auto pos = ref->GetPosition();
                return Anchor{ pos.x, pos.y, WorldOf(cell), "own exterior position" };
            }
            // Interior: breadth-first over the doors of the cell (and of the cells they lead to) until a door's far
            // side is in an exterior cell. The linked door's own position is then the exterior stand-in.
            std::deque<std::pair<RE::TESObjectCELL*, int>> queue;
            std::set<RE::FormID>                           seen;
            queue.emplace_back(cell, 0);
            seen.insert(cell->GetFormID());
            while (!queue.empty()) {
                auto [c, depth] = queue.front();
                queue.pop_front();
                if (depth >= 6) {
                    continue;
                }
                std::optional<Anchor>           found;
                std::vector<RE::TESObjectCELL*> next;
                c->ForEachReference([&](RE::TESObjectREFR* door) {
                    if (!door || found) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    auto* tele = door->extraList.GetByType<RE::ExtraTeleport>();
                    if (!tele || !tele->teleportData) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    auto linked = tele->teleportData->linkedDoor.get();
                    if (!linked) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    auto* lc = CellOf(linked.get());
                    if (!lc) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    if (lc->IsExteriorCell()) {
                        const auto pos = linked->GetPosition();
                        found          = Anchor{ pos.x, pos.y, WorldOf(lc),
                            std::format("exterior door {:08X} reached through {} door(s) from {}", linked->GetFormID(),
                                depth + 1, CellName(cell)) };
                    } else if (seen.insert(lc->GetFormID()).second) {
                        next.push_back(lc);
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                if (found) {
                    return found;
                }
                for (auto* n : next) {
                    queue.emplace_back(n, depth + 1);
                }
            }
        } catch (...) {
            spdlog::error("Prison: exception finding exterior anchor");
        }
        return std::nullopt;
    }

    void Discover()
    {
        spdlog::info("Prison discovery: start ({} holds)", Holds().size());
        for (const auto& def : Holds()) {
            const auto  r          = Resolve(def);
            std::string anchorText = "tamriel anchor=missing from GameData";
            if (auto a = TamrielAnchor(def)) {
                anchorText = std::format("tamriel anchor=({:.0f},{:.0f})", a->x, a->y);
            }
            spdlog::info("Prison: hold={} faction={:08X} {} | chest[{}]={} | jail[{}]={} | {}", def.name, def.factionId,
                r.faction ? "found" : "MISSING", r.chestSource, r.chest ? DescribeRef(r.chest) : "missing", r.jailSource,
                r.jail ? DescribeRef(r.jail) : "missing", anchorText);
        }
        const auto markers = CollectWakeMarkers();
        spdlog::info("Prison discovery: Tamriel wake markers: table {} / found {} / usable {} / no cell {} / moved {} / live {} (other mods {})",
            markers.tableTotal, markers.tableFound, markers.usable.size(), markers.noCell, markers.moved, markers.liveFound, markers.liveExtra);
        spdlog::info("Prison discovery: done");
    }
}
