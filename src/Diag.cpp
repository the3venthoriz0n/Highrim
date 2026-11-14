#include "PCH.h"

#include "Diag.h"

#include "Forms.h"

#include <chrono>
#include <vector>

namespace Highrim::Diag
{
    namespace
    {
        struct Pending
        {
            std::uint32_t localId;
            std::string   label;
            double        due;
        };
        std::vector<Pending> g_pending;  // game thread only

        double RealSeconds()
        {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        void Check(const Pending& p)
        {
            auto* player  = RE::PlayerCharacter::GetSingleton();
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!player || !handler) {
                return;
            }
            auto* spell = handler->LookupForm<RE::SpellItem>(p.localId, Forms::kPluginName);
            if (!spell) {
                spdlog::error("Highrim {} active: spell {:06X} not found in {}", p.label, p.localId, Forms::kPluginName);
                return;
            }
            auto* target = player->AsMagicTarget();
            auto* list   = target ? target->GetActiveEffectList() : nullptr;
            int   active = 0;
            if (list) {
                for (auto* ae : *list) {
                    if (!ae || ae->spell != spell) {
                        continue;
                    }
                    if (ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
                        continue;
                    }
                    ++active;
                }
            }
            const int expected = static_cast<int>(spell->effects.size());
            if (active < expected) {
                spdlog::error("Highrim {} active: {} of {} ('{}' {:08X}) - the effect is missing from the player", p.label, active,
                    expected, spell->GetName(), spell->GetFormID());
            } else {
                spdlog::info("Highrim {} active: {} of {} ('{}' {:08X})", p.label, active, expected, spell->GetName(),
                    spell->GetFormID());
            }
        }
    }

    void Schedule(std::uint32_t localSpellId, const std::string& label, double delaySeconds)
    {
        g_pending.push_back({ localSpellId, label, RealSeconds() + delaySeconds });
    }

    void Tick()
    {
        if (g_pending.empty()) {
            return;
        }
        const double now = RealSeconds();
        for (std::size_t i = 0; i < g_pending.size();) {
            if (now >= g_pending[i].due) {
                const Pending p = g_pending[i];
                g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
                try {
                    Check(p);
                } catch (...) {
                    spdlog::error("Highrim {} active: exception while checking", p.label);
                }
            } else {
                ++i;
            }
        }
    }

    void Clear()
    {
        g_pending.clear();
    }
}
