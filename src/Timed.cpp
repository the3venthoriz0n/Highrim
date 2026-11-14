#include "PCH.h"

#include "Timed.h"

#include "Core.h"
#include "Forms.h"

namespace Highrim::Timed
{
    namespace
    {
        bool Live(const RE::ActiveEffect* ae)
        {
            return ae && !ae->flags.any(RE::ActiveEffect::Flag::kDispelled);
        }

        RE::BSSimpleList<RE::ActiveEffect*>* EffectList()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* target = player ? player->AsMagicTarget() : nullptr;
            return target ? target->GetActiveEffectList() : nullptr;
        }
    }

    RE::SpellItem* Lookup(std::uint32_t localId)
    {
        auto* handler = RE::TESDataHandler::GetSingleton();
        return handler ? handler->LookupForm<RE::SpellItem>(localId, Forms::kPluginName) : nullptr;
    }

    int CountLive(RE::SpellItem* spell)
    {
        auto* list = EffectList();
        if (!spell || !list) return 0;
        int count = 0;
        for (auto* ae : *list) {
            if (Live(ae) && ae->spell == spell) ++count;
        }
        return count;
    }

    bool Cast(RE::SpellItem* spell)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!spell || !player) return false;
        auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
        if (!caster) {
            spdlog::error("Timed: no instant caster on the player, '{}' not cast", spell->GetName());
            return false;
        }
        caster->CastSpellImmediate(spell, false, player, 1.0f, false, 0.0f, nullptr);
        return true;
    }

    int Dispel(RE::SpellItem* spell)
    {
        auto* list = EffectList();
        if (!spell || !list) return 0;
        int count = 0;
        for (auto* ae : *list) {
            if (Live(ae) && ae->spell == spell) {
                ae->Dispel(true);
                ++count;
            }
        }
        return count;
    }

    int SetRemaining(RE::SpellItem* spell, double remainingSeconds)
    {
        auto* list = EffectList();
        if (!spell || !list) return 0;
        int count = 0;
        for (auto* ae : *list) {
            if (Live(ae) && ae->spell == spell) {
                ae->duration = Core::TimedDuration(ae->elapsedSeconds, remainingSeconds);
                ++count;
            }
        }
        return count;
    }

    double RemainingSeconds(RE::SpellItem* spell)
    {
        auto* list = EffectList();
        if (!spell || !list) return -1.0;
        bool   any      = false;
        double smallest = 0.0;
        for (auto* ae : *list) {
            if (Live(ae) && ae->spell == spell) {
                const double left = std::max(0.0, static_cast<double>(ae->duration) - static_cast<double>(ae->elapsedSeconds));
                smallest          = any ? std::min(smallest, left) : left;
                any               = true;
            }
        }
        return any ? smallest : -1.0;
    }

    bool SetEffectMagnitudes(RE::SpellItem* spell, const float* magnitudes, std::size_t count)
    {
        if (!spell || spell->effects.size() != count) return false;
        for (std::size_t i = 0; i < count; ++i) {
            if (!spell->effects[i]) return false;
            spell->effects[i]->effectItem.magnitude = magnitudes[i];
        }
        return true;
    }
}
