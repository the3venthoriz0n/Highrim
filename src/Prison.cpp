#include "PCH.h"

#include "Prison.h"

#include "Forms.h"

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
            { "Whiterun",   "Whiterun",   0x000267EA, 0x000267E8, 0x000267E5, Forms::HighrimNoteWhiterun },
            { "Eastmarch",  "Windhelm",   0x000267E3, 0x0003EF10, 0x0003EF18, Forms::HighrimNoteEastmarch },
            { "Rift",       "Riften",     0x0002816B, 0x000A8F33, 0x0003EF58, Forms::HighrimNoteRift },
            { "Haafingar",  "Solitude",   0x00029DB0, 0x0003EEFF, 0x0003EF19, Forms::HighrimNoteHaafingar },
            { "Hjaalmarch", "Morthal",    0x0002816D, 0x0003EF09, 0x0003EF1A, Forms::HighrimNoteHjaalmarch },
            { "Reach",      "Markarth",   0x0002816C, 0x0003EF03, 0x0003EF57, Forms::HighrimNoteReach },
            { "Falkreath",  "Falkreath",  0x00028170, 0x000EF437, 0x0003EF17, Forms::HighrimNoteFalkreath },
            { "Pale",       "Dawnstar",   0x0002816E, 0x0003EF12, 0x0003EF1B, Forms::HighrimNotePale },
            { "Winterhold", "Winterhold", 0x0002816F, 0,          0x0003EF59, Forms::HighrimNoteWinterhold },
        };
        return holds;
    }

    namespace
    {
        RE::TESWorldSpace* WorldOf(RE::TESObjectCELL* cell)
        {
            return cell ? cell->GetRuntimeData().worldSpace : nullptr;
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
            auto*      cell = ref->GetParentCell();
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

    std::vector<Marker> WorldMarkers(RE::TESWorldSpace* world)
    {
        std::vector<Marker> out;
        try {
            if (!world || !world->persistentCell) {
                return out;
            }
            world->persistentCell->ForEachReference([&](RE::TESObjectREFR* ref) {
                if (!ref) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
                if (extra && extra->mapData) {
                    const char* n   = extra->mapData->locationName.GetFullName();
                    const auto  pos = ref->GetPosition();
                    out.push_back({ ref, pos.x, pos.y, n ? n : "",
                        extra->mapData->flags.any(RE::MapMarkerData::Flag::kVisible) });
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        } catch (...) {
            spdlog::error("Prison: exception reading map markers");
        }
        return out;
    }

    std::optional<Anchor> ExteriorAnchor(RE::TESObjectREFR* ref)
    {
        try {
            if (!ref) {
                return std::nullopt;
            }
            auto* cell = ref->GetParentCell();
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
                    auto* lc = linked->GetParentCell();
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
            std::string anchorText = "anchor=unknown";
            if (r.jail) {
                if (auto a = ExteriorAnchor(r.jail)) {
                    anchorText = std::format("anchor=({:.0f},{:.0f}) world={:08X} how=[{}]", a->x, a->y,
                        a->world ? a->world->GetFormID() : 0u, a->how);
                }
            }
            spdlog::info("Prison: hold={} faction={:08X} {} | chest[{}]={} | jail[{}]={} | {}", def.name, def.factionId,
                r.faction ? "found" : "MISSING", r.chestSource, r.chest ? DescribeRef(r.chest) : "missing", r.jailSource,
                r.jail ? DescribeRef(r.jail) : "missing", anchorText);
        }
        spdlog::info("Prison discovery: done");
    }
}
