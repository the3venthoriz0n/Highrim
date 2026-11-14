#include "PCH.h"

#include "LostGear.h"

#include "Core.h"
#include "Diag.h"
#include "Forms.h"
#include "Prison.h"

#include <array>
#include <chrono>

namespace Highrim::LostGear
{
    namespace
    {
        // One ability per hold, in the order of Prison::Holds() (checked by tests/test_sources.py).
        constexpr std::array<std::uint32_t, 9> kAbilityIds{
            Forms::HighrimLostGearWhiterun, Forms::HighrimLostGearEastmarch, Forms::HighrimLostGearRift,
            Forms::HighrimLostGearHaafingar, Forms::HighrimLostGearHjaalmarch, Forms::HighrimLostGearReach,
            Forms::HighrimLostGearFalkreath, Forms::HighrimLostGearPale, Forms::HighrimLostGearWinterhold
        };

        // Written by the co-save callbacks (game thread in practice) and the poller.
        std::atomic<int>    g_hold{ -1 };
        std::atomic<double> g_expiry{ 0.0 };
        // Game thread only.
        RE::FormID g_chestCell = 0;
        RE::FormID g_jailCell  = 0;
        double     g_nextCheck = 0.0;
        bool       g_needApply = false;

        double RealSeconds()
        {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        double NowHours()
        {
            auto* calendar = RE::Calendar::GetSingleton();
            if (!calendar || !calendar->gameDaysPassed) {
                return -1.0;
            }
            return static_cast<double>(calendar->gameDaysPassed->value) * 24.0;
        }

        RE::SpellItem* Lookup(std::size_t index)
        {
            auto* handler = RE::TESDataHandler::GetSingleton();
            return (handler && index < kAbilityIds.size()) ? handler->LookupForm<RE::SpellItem>(kAbilityIds[index], Forms::kPluginName) : nullptr;
        }

        RE::FormID CellOf(RE::TESObjectREFR* ref)
        {
            if (!ref) {
                return 0;
            }
            auto* cell = ref->GetParentCell();
            if (!cell) {
                cell = ref->GetSaveParentCell();
            }
            return cell ? cell->GetFormID() : 0;
        }

        // The cells that end the reminder: the confiscation chest's and the jail marker's.
        void ResolveCells(int hold)
        {
            g_chestCell = 0;
            g_jailCell  = 0;
            const auto& holds = Prison::Holds();
            if (hold < 0 || static_cast<std::size_t>(hold) >= holds.size()) {
                return;
            }
            const auto r = Prison::Resolve(holds[static_cast<std::size_t>(hold)]);
            g_chestCell  = CellOf(r.chest);
            g_jailCell   = CellOf(r.jail);
            spdlog::info("Lost Gear: {} prison cells: chest {:08X}, jail {:08X}", holds[static_cast<std::size_t>(hold)].name, g_chestCell, g_jailCell);
        }

        void RemoveAll(RE::PlayerCharacter* player)
        {
            for (std::size_t i = 0; i < kAbilityIds.size(); ++i) {
                auto* spell = Lookup(i);
                if (spell && player->HasSpell(spell)) {
                    const bool removed = player->RemoveSpell(spell);
                    spdlog::info("Lost Gear: removed the {} ability, RemoveSpell -> {}", spell->GetName(), removed);
                }
            }
        }

        void Apply(RE::PlayerCharacter* player, int hold)
        {
            RemoveAll(player);
            auto* spell = Lookup(static_cast<std::size_t>(hold));
            if (!spell) {
                spdlog::error("Lost Gear: ability for hold {} not found in {}", hold, Forms::kPluginName);
                return;
            }
            const bool added = player->AddSpell(spell);
            spdlog::info("Lost Gear: '{}' added -> {} (expires at game hour {:.2f})", spell->GetName(), added, g_expiry.load());
            Diag::Schedule(kAbilityIds[static_cast<std::size_t>(hold)], "lost gear");
        }

        void End(RE::PlayerCharacter* player, const char* why)
        {
            spdlog::info("Lost Gear: reminder ended ({})", why);
            if (player) {
                RemoveAll(player);
            }
            g_hold   = -1;
            g_expiry = 0.0;
            g_chestCell = g_jailCell = 0;
        }
    }

    void Begin(int holdIndex)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        const double now = NowHours();
        if (!player || holdIndex < 0 || static_cast<std::size_t>(holdIndex) >= kAbilityIds.size() || now < 0.0) {
            spdlog::error("Lost Gear: cannot begin (player {}, hold {}, hour {:.2f})", player != nullptr, holdIndex, now);
            return;
        }
        g_hold   = holdIndex;
        g_expiry = now + Core::kLostGearHours;
        ResolveCells(holdIndex);
        Apply(player, holdIndex);
        g_nextCheck = RealSeconds() + 5.0;  // the player is not in the prison cell yet; wait a moment before checking
    }

    int GetHold()
    {
        return g_hold.load();
    }

    double GetExpiryHours()
    {
        return g_expiry.load();
    }

    void Set(int holdIndex, double expiryHours)
    {
        g_hold   = holdIndex;
        g_expiry = expiryHours;
    }

    void OnGameLoaded()
    {
        g_chestCell = g_jailCell = 0;
        g_nextCheck = 0.0;
        g_needApply = g_hold.load() >= 0;
        spdlog::info("Lost Gear: loaded, pending hold {}, expiry {:.2f}", g_hold.load(), g_expiry.load());
    }

    void Tick()
    {
        try {
            const int hold = g_hold.load();
            auto*     player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded() || !player->GetParentCell()) {
                return;
            }
            const double real = RealSeconds();
            if (hold < 0) {
                // Nothing pending: strip a stray ability once after a load (the save may carry one from an ended reminder).
                if (g_needApply) {
                    g_needApply = false;
                    RemoveAll(player);
                }
                return;
            }
            if (real < g_nextCheck) {
                return;
            }
            g_nextCheck = real + 2.0;
            const double now = NowHours();
            if (now < 0.0) {
                return;
            }
            if (Core::LostGearExpired(now, g_expiry.load())) {
                End(player, "three game days passed");
                return;
            }
            if (g_needApply) {
                g_needApply = false;
                ResolveCells(hold);
                Apply(player, hold);
                return;
            }
            if (g_chestCell == 0 && g_jailCell == 0) {
                ResolveCells(hold);
            }
            const auto here = player->GetParentCell()->GetFormID();
            if (here != 0 && (here == g_chestCell || here == g_jailCell)) {
                End(player, "the player reached the prison");
            }
        } catch (...) {
            spdlog::error("Lost Gear: exception in tick");
        }
    }
}
