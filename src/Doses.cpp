#include "PCH.h"

#include "Doses.h"

#include "Blackout.h"
#include "Config.h"
#include "Core.h"
#include "Diag.h"
#include "Forms.h"
#include "Ragdoll.h"
#include "State.h"

#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace Highrim::Doses
{
    namespace
    {
        // A dose-triggered blackout first knocks the player down; it starts Core::kRagdollFirstSeconds later.
        // Screen effects are re-triggered this often while a high lasts (image space modifiers fade).
        constexpr double kVisualPeriodSeconds = 10.0;

        // All of this is guarded by g_mutex. Stacks live in memory only.
        std::mutex            g_mutex;
        Core::ComedownTimers  g_comedowns{};
        Core::DiagnosticTimers g_diagnostics{};
        std::optional<double> g_overdoseDue;  // real seconds: when the knock-down is due (waits while a menu is open)
        std::optional<double> g_blackoutDue;  // real seconds: when the blackout itself starts (set by the knock)
        // What the pending overdose will be once the knock-down is over (Faint or Full); kNone when nothing is pending.
        Core::OverdoseOutcome g_pendingOutcome = Core::OverdoseOutcome::kNone;
        std::mt19937          g_rng{ std::random_device{}() };  // the overdose rolls (used under g_mutex)
        Core::DoseStacks      g_stacks{};
        Core::DoseTotals      g_totals{};     // doses of any drug, for the blackout count
        std::array<int, 3>    g_wantedLevel{};               // stack ability level each drug should carry (0 = none)
        std::array<int, 3>    g_appliedLevel{ -1, -1, -1 };  // what the player carries; -1 = unknown, always re-sync
        std::array<double, 3> g_strength{ 0.6, 0.6, 0.6 };
        // "Shaky and Suspicious" (Speech and Sneak 0) while any high lasts. wanted is computed under g_mutex in Tick,
        // applied on the game thread; -1 = unknown (always re-sync after a load, new game, revert or blackout start).
        bool                  g_shakyWanted  = false;
        int                   g_shakyApplied = -1;
        std::array<std::optional<double>, 3> g_visualDue{};  // real seconds; set while that drug's high lasts
        std::array<bool, 3>   g_visualActive{};              // a screen effect was triggered and not stopped yet
        std::array<bool, 3>   g_visualStop{};                // the game thread must stop it (high ended, load, blackout)
        std::uint32_t         g_epoch = 0;                   // bumped on every reset so stale game-thread work is dropped

        // Overdose risk indicator (Task 16): the tier the player should carry (Core::RiskTier as int) and what the game thread
        // applied (-1 = unknown, always re-sync); and the Elevated / High screen warning, re-triggered like the drug visuals.
        int                   g_riskWanted  = 0;
        int                   g_riskApplied = -1;
        std::optional<double> g_riskVisualDue;      // real seconds
        double                g_riskStrength = 0.0;  // strength the warning should run at; 0 = none
        double                g_riskLogged   = 0.0;  // strength of the last "started" log line, so re-triggers stay quiet
        bool                  g_riskVisualActive = false;
        constexpr std::array<std::uint32_t, 3> kRiskIds{ Forms::HighrimOverdoseLow, Forms::HighrimOverdoseElevated, Forms::HighrimOverdoseHigh };

        // Comedown screen effect (one black-and-white IMAD shared by the three drugs): runs until g_comedownEnd, then
        // the game thread stops it. A later comedown restarts it with its own strength and a new end.
        std::optional<double> g_comedownEnd;     // real seconds
        bool                  g_comedownActive = false;
        bool                  g_comedownStop   = false;  // load, new game, blackout start: stop it now

        // Dreamcap reveal life: the plugin casts vanilla Detect Life itself (potion effects never applied it).
        std::optional<double> g_detectNext;      // real seconds: next cast, set while a Dreamcap high lasts
        std::optional<double> g_detectCheckAt;   // real seconds: log the active effects once after a cast

        // The vanilla scroll whose four effects are 001019D8..DB (TGDetectLifeScroll); the player's own Detect Life spell
        // (DetectLife, 000211EE) is the fallback.
        constexpr RE::FormID kDetectLifeScroll = 0x001019AE;
        constexpr RE::FormID kDetectLifeSpell  = 0x000211EE;

        constexpr std::array<std::array<std::uint32_t, 3>, 3> kStackIds{ {
            { Forms::HighrimStackRimerush1, Forms::HighrimStackRimerush2, Forms::HighrimStackRimerush3 },
            { Forms::HighrimStackVelvetHaze1, Forms::HighrimStackVelvetHaze2, Forms::HighrimStackVelvetHaze3 },
            { Forms::HighrimStackDreamcap1, Forms::HighrimStackDreamcap2, Forms::HighrimStackDreamcap3 },
        } };
        // Rimerush (index 0) has no screen effect during the high (Task 18, Core::HasHighVisual); Velvet Haze uses our own IMAD, see LookupImod.
        constexpr std::array<std::uint32_t, 3> kImodIds{ 0u, 0u, Forms::kImodHallucinogen };

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

        const char* DrugName(Core::DrugType t)
        {
            switch (t) {
            case Core::DrugType::kStimulant: return "Rimerush";
            case Core::DrugType::kDepressant: return "Velvet Haze";
            default: return "Dreamcap";
            }
        }

        std::uint32_t ComedownLocalId(Core::DrugType t)
        {
            switch (t) {
            case Core::DrugType::kStimulant: return Forms::HighrimComedownRimerush;
            case Core::DrugType::kDepressant: return Forms::HighrimComedownVelvetHaze;
            default: return Forms::HighrimComedownDreamcap;
            }
        }

        // Which of our three drugs is this form? Resolved through the plugin so the load order index never matters.
        std::optional<Core::DrugType> DrugFromForm(RE::FormID formID)
        {
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!handler) {
                return std::nullopt;
            }
            for (std::uint32_t local : { Forms::HighrimRimerush, Forms::HighrimVelvetHaze, Forms::HighrimDreamcap }) {
                auto* form = handler->LookupForm(local, Forms::kPluginName);
                if (form && form->GetFormID() == formID) {
                    return Core::DrugFromLocalId(local);
                }
            }
            return std::nullopt;
        }

        std::uint32_t DrugLocalId(Core::DrugType t)
        {
            switch (t) {
            case Core::DrugType::kStimulant: return Forms::HighrimRimerush;
            case Core::DrugType::kDepressant: return Forms::HighrimVelvetHaze;
            default: return Forms::HighrimDreamcap;
            }
        }

        const char* SafeName(RE::TESForm* form, bool editorId)
        {
            if (!form) {
                return "(none)";
            }
            const char* text = editorId ? form->GetFormEditorID() : form->GetName();
            return (text && *text) ? text : "(unnamed)";
        }

        // One info line per active effect on the player, then how many of the potion's effects are active.
        void LogActiveEffects(Core::DrugType type, int pass)
        {
            auto* player  = RE::PlayerCharacter::GetSingleton();
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!player || !handler) {
                return;
            }
            auto* potion = handler->LookupForm<RE::AlchemyItem>(DrugLocalId(type), Forms::kPluginName);
            auto* target = player->AsMagicTarget();
            auto* list   = target ? target->GetActiveEffectList() : nullptr;
            spdlog::info("Effects ({}): pass {} for {}", pass == 1 ? "0.5 s" : "5 s", pass, DrugName(type));
            int                      active = 0;
            std::vector<RE::FormID>  activeBases;
            if (list) {
                for (auto* ae : *list) {
                    if (!ae || !ae->effect || !ae->effect->baseEffect) {
                        continue;
                    }
                    auto* base = ae->effect->baseEffect;
                    const bool off = ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled);
                    spdlog::info("  effect {:08X} {} '{}' mag {:.2f} dur {:.1f} elapsed {:.1f} inactive={} dispelled={}",
                        base->GetFormID(), SafeName(base, true), SafeName(base, false), ae->magnitude, ae->duration,
                        ae->elapsedSeconds, ae->flags.any(RE::ActiveEffect::Flag::kInactive),
                        ae->flags.any(RE::ActiveEffect::Flag::kDispelled));
                    if (potion && !off) {
                        for (auto* fx : potion->effects) {
                            if (fx && fx->baseEffect == base) {
                                ++active;
                                activeBases.push_back(base->GetFormID());
                                break;
                            }
                        }
                    }
                }
            } else {
                spdlog::warn("  no active effect list on the player");
            }
            const int expected = potion ? static_cast<int>(potion->effects.size()) : 0;
            // Task 17: effects flagged Hide in UI (the 18 per-skill ones) are still applied and still counted in `expected`.
            int hiddenExpected = 0;
            if (potion) {
                for (auto* fx : potion->effects) {
                    if (fx && fx->baseEffect && fx->baseEffect->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI)) {
                        ++hiddenExpected;
                    }
                }
            }
            spdlog::info("Highrim effects active: {} of {} expected for {} ({} visible, {} hidden in the UI)", active, expected,
                DrugName(type), expected - hiddenExpected, hiddenExpected);
            std::string missing;
            if (potion) {
                for (auto* fx : potion->effects) {
                    if (fx && fx->baseEffect &&
                        std::find(activeBases.begin(), activeBases.end(), fx->baseEffect->GetFormID()) == activeBases.end()) {
                        if (!missing.empty()) missing += ", ";
                        missing += std::string(SafeName(fx->baseEffect, true)) + " ('" + SafeName(fx->baseEffect, false) + "')";
                    }
                }
            }
            spdlog::info("Highrim effects missing: {}", missing.empty() ? "none" : missing);
        }

        void CastComedown(Core::DrugType type)
        {
            auto* player  = RE::PlayerCharacter::GetSingleton();
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!player || !handler || !player->Is3DLoaded() || player->IsDead()) {
                spdlog::info("Comedown: {} skipped, player not ready", DrugName(type));
                return;
            }
            auto* spell = handler->LookupForm<RE::SpellItem>(ComedownLocalId(type), Forms::kPluginName);
            if (!spell) {
                spdlog::error("Comedown: spell for {} not found in {}", DrugName(type), Forms::kPluginName);
                return;
            }
            auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            if (!caster) {
                spdlog::error("Comedown: no instant caster on the player");
                return;
            }
            caster->CastSpellImmediate(spell, false, player, 1.0f, false, 0.0f, nullptr);
            spdlog::info("Comedown: cast {} comedown", DrugName(type));
        }

        RE::SpellItem* LookupStack(Core::DrugType type, int level)
        {
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!handler || level < 1 || level > Core::kMaxStackLevel) {
                return nullptr;
            }
            return handler->LookupForm<RE::SpellItem>(
                kStackIds[static_cast<std::size_t>(type)][static_cast<std::size_t>(level - 1)], Forms::kPluginName);
        }

        // Game thread. Makes the player carry exactly the stack ability for each drug's wanted level (none for 0),
        // never more than one per drug. Remembers what it did only if no reset happened meanwhile.
        void SyncStackAbilities()
        {
            std::array<int, 3> wanted{};
            std::array<int, 3> applied{};
            std::uint32_t      epoch = 0;
            {
                std::lock_guard lock(g_mutex);
                wanted  = g_wantedLevel;
                applied = g_appliedLevel;
                epoch   = g_epoch;
            }
            if (wanted == applied) {
                return;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* ui     = RE::UI::GetSingleton();
            if (!player || !player->Is3DLoaded() || !player->GetParentCell() || !ui ||
                ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                return;
            }
            auto result = applied;
            for (std::size_t i = 0; i < 3; ++i) {
                if (wanted[i] == applied[i]) {
                    continue;
                }
                const auto type = static_cast<Core::DrugType>(i);
                bool       ok   = true;
                for (int level = 1; level <= Core::kMaxStackLevel; ++level) {
                    auto* spell = LookupStack(type, level);
                    if (!spell) {
                        spdlog::error("Stack: ability {} level {} not found in {}", DrugName(type), level, Forms::kPluginName);
                        ok = false;
                    } else if (player->HasSpell(spell)) {
                        const bool removed = player->RemoveSpell(spell);
                        spdlog::info("Stack: removed {} level {} ability, RemoveSpell -> {}", DrugName(type), level, removed);
                    }
                }
                if (!ok) {
                    continue;  // retried on the next tick
                }
                if (wanted[i] > 0) {
                    auto* spell = LookupStack(type, wanted[i]);
                    if (!spell) {
                        continue;
                    }
                    const bool added = player->AddSpell(spell);
                    spdlog::info("Stack: {} level {} ability, AddSpell -> {}", DrugName(type), wanted[i], added);
                    if (!added && !player->HasSpell(spell)) {
                        continue;
                    }
                }
                result[i] = wanted[i];
            }
            std::lock_guard lock(g_mutex);
            if (epoch == g_epoch) {
                g_appliedLevel = result;
            }
        }

        // Game thread. Makes the player carry "Shaky and Suspicious" exactly while a high lasts, never twice.
        void SyncShaky()
        {
            bool          wanted  = false;
            int           applied = -1;
            std::uint32_t epoch   = 0;
            {
                std::lock_guard lock(g_mutex);
                wanted  = g_shakyWanted;
                applied = g_shakyApplied;
                epoch   = g_epoch;
            }
            if (applied == (wanted ? 1 : 0)) {
                return;
            }
            auto* player  = RE::PlayerCharacter::GetSingleton();
            auto* ui      = RE::UI::GetSingleton();
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!player || !handler || !player->Is3DLoaded() || !player->GetParentCell() || !ui ||
                ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                return;
            }
            auto* spell = handler->LookupForm<RE::SpellItem>(Forms::HighrimShaky, Forms::kPluginName);
            if (!spell) {
                spdlog::error("Shaky: ability not found in {}", Forms::kPluginName);
                return;
            }
            const bool has = player->HasSpell(spell);
            if (wanted && !has) {
                const bool added = player->AddSpell(spell);
                spdlog::info("Shaky: high started, 'Shaky and Suspicious' added, AddSpell -> {}", added);
                if (!added && !player->HasSpell(spell)) {
                    return;
                }
            } else if (!wanted && has) {
                const bool removed = player->RemoveSpell(spell);
                spdlog::info("Shaky: no high, 'Shaky and Suspicious' removed, RemoveSpell -> {}", removed);
                if (!removed && player->HasSpell(spell)) {
                    return;
                }
            }
            std::lock_guard lock(g_mutex);
            if (epoch == g_epoch) {
                g_shakyApplied = wanted ? 1 : 0;
            }
        }

        // Velvet Haze uses our own blur-free IMAD (in the plugin); the others are vanilla records.
        RE::TESImageSpaceModifier* LookupImod(Core::DrugType type)
        {
            if (type == Core::DrugType::kDepressant) {
                auto* handler = RE::TESDataHandler::GetSingleton();
                return handler ? handler->LookupForm<RE::TESImageSpaceModifier>(Forms::HighrimImodVelvet, Forms::kPluginName)
                               : nullptr;
            }
            return RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(kImodIds[static_cast<std::size_t>(type)]);
        }

        void TriggerVisual(Core::DrugType type, double strength)
        {
            if (!Core::HasHighVisual(type)) return;  // Rimerush: no screen effect while high
            auto* imod = LookupImod(type);
            if (!imod) {
                spdlog::error("Visual: image space modifier for {} not found", DrugName(type));
                return;
            }
            RE::ImageSpaceModifierInstanceForm::Trigger(imod, static_cast<float>(strength), nullptr);
            {
                std::lock_guard lock(g_mutex);
                g_visualActive[static_cast<std::size_t>(type)] = true;
            }
            spdlog::debug("Visual: {} image space modifier {:08X} at strength {:.2f}", DrugName(type), imod->GetFormID(), strength);
        }

        void StopVisual(Core::DrugType type)
        {
            auto* imod = LookupImod(type);
            if (!imod) {
                spdlog::error("Visual: cannot stop the {} image space modifier, not found", DrugName(type));
                return;
            }
            RE::ImageSpaceModifierInstanceForm::Stop(imod);
            spdlog::info("Visual: stopped {} image space modifier {:08X}", DrugName(type), imod->GetFormID());
        }

        // Game thread. How many active effects on the player come from this magic item.
        int ActiveEffectsFrom(RE::MagicItem* item)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* target = player ? player->AsMagicTarget() : nullptr;
            auto* list   = target ? target->GetActiveEffectList() : nullptr;
            int   count  = 0;
            if (list) {
                for (auto* ae : *list) {
                    if (ae && ae->spell == item && !ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
                        ++count;
                    }
                }
            }
            return count;
        }

        RE::MagicItem* DetectLifeItem()
        {
            if (auto* scroll = RE::TESForm::LookupByID<RE::ScrollItem>(kDetectLifeScroll)) {
                return scroll;
            }
            return RE::TESForm::LookupByID<RE::SpellItem>(kDetectLifeSpell);
        }

        void CastDetectLife()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded() || player->IsDead()) {
                return;
            }
            auto* item = DetectLifeItem();
            if (!item) {
                spdlog::error("Detect Life: neither the vanilla scroll {:08X} nor the spell {:08X} was found", kDetectLifeScroll, kDetectLifeSpell);
                return;
            }
            auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            if (!caster) {
                spdlog::error("Detect Life: no instant caster on the player");
                return;
            }
            caster->CastSpellImmediate(item, false, player, 1.0f, false, 0.0f, nullptr);
            const int active = ActiveEffectsFrom(item);
            spdlog::info("Detect Life: cast (spell {:08X}) -> active effects {} of {}", item->GetFormID(), active, item->effects.size());
            std::lock_guard lock(g_mutex);
            g_detectCheckAt = RealSeconds() + 0.5;
        }

        void LogDetectLifeCheck()
        {
            if (auto* item = DetectLifeItem()) {
                spdlog::info("Detect Life: 0.5 s after the cast (spell {:08X}) -> active effects {}", item->GetFormID(), ActiveEffectsFrom(item));
            }
        }

        // Game thread. Makes the player carry exactly the Overdose Risk ability of the wanted tier (none for 0), never two.
        // A tier that rose since the last sync says so on the HUD (after a load nothing was applied and the dose total is
        // empty, so any tier above none is a rise from none).
        void SyncRiskAbility()
        {
            int           wanted  = 0;
            int           applied = -1;
            std::uint32_t epoch   = 0;
            {
                std::lock_guard lock(g_mutex);
                wanted  = g_riskWanted;
                applied = g_riskApplied;
                epoch   = g_epoch;
            }
            if (wanted == applied) {
                return;
            }
            auto* player  = RE::PlayerCharacter::GetSingleton();
            auto* ui      = RE::UI::GetSingleton();
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!player || !handler || !player->Is3DLoaded() || !player->GetParentCell() || !ui ||
                ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                return;
            }
            std::array<RE::SpellItem*, 3> spells{};
            for (std::size_t i = 0; i < 3; ++i) {
                spells[i] = handler->LookupForm<RE::SpellItem>(kRiskIds[i], Forms::kPluginName);
                if (!spells[i]) {
                    spdlog::error("Overdose risk: ability {} not found in {}", i + 1, Forms::kPluginName);
                    return;  // retried on the next tick
                }
            }
            for (std::size_t i = 0; i < 3; ++i) {
                if (static_cast<int>(i) + 1 != wanted && player->HasSpell(spells[i])) {
                    const bool removed = player->RemoveSpell(spells[i]);
                    spdlog::info("Overdose risk: removed '{}', RemoveSpell -> {}", Core::RiskTierName(static_cast<Core::RiskTier>(i + 1)), removed);
                    if (!removed && player->HasSpell(spells[i])) {
                        return;
                    }
                }
            }
            if (wanted >= 1 && wanted <= 3) {
                auto* spell = spells[static_cast<std::size_t>(wanted - 1)];
                if (!player->HasSpell(spell)) {
                    const bool added = player->AddSpell(spell);
                    spdlog::info("Overdose risk: tier {} -> {} ('{}'), AddSpell -> {}", std::max(applied, 0), wanted,
                        Core::RiskTierName(static_cast<Core::RiskTier>(wanted)), added);
                    if (!added && !player->HasSpell(spell)) {
                        return;
                    }
                }
                if (Core::RiskTierRose(static_cast<Core::RiskTier>(std::max(applied, 0)), static_cast<Core::RiskTier>(wanted))) {
                    RE::SendHUDMessage::ShowHUDMessage(Core::RiskRiseMessage(static_cast<Core::RiskTier>(wanted)).c_str(), nullptr, false);
                }
            }
            std::lock_guard lock(g_mutex);
            if (epoch == g_epoch) {
                g_riskApplied = wanted;
            }
        }

        RE::TESImageSpaceModifier* LookupRiskImod()
        {
            auto* handler = RE::TESDataHandler::GetSingleton();
            return handler ? handler->LookupForm<RE::TESImageSpaceModifier>(Forms::HighrimImodOverdose, Forms::kPluginName) : nullptr;
        }

        void TriggerRiskVisual(double strength)
        {
            auto* imod = LookupRiskImod();
            if (!imod) {
                spdlog::error("Overdose visual: image space modifier {:06X} not found in {}", Forms::HighrimImodOverdose, Forms::kPluginName);
                return;
            }
            RE::ImageSpaceModifierInstanceForm::Trigger(imod, static_cast<float>(strength), nullptr);
            bool fresh = false;
            {
                std::lock_guard lock(g_mutex);
                g_riskVisualActive = true;
                fresh              = g_riskLogged != strength;
                g_riskLogged       = strength;
            }
            if (fresh) {
                spdlog::info("Overdose visual: started ({:08X}) at strength {:.2f}, re-triggered about every {:.0f} s", imod->GetFormID(),
                    strength, kVisualPeriodSeconds);
            } else {
                spdlog::debug("Overdose visual: re-triggered at strength {:.2f}", strength);
            }
        }

        void StopRiskVisual()
        {
            auto* imod = LookupRiskImod();
            if (imod) {
                RE::ImageSpaceModifierInstanceForm::Stop(imod);
            }
            {
                std::lock_guard lock(g_mutex);
                g_riskLogged = 0.0;
            }
            spdlog::info("Overdose visual: stopped{}", imod ? "" : " - image space modifier not found");
        }

        void StartComedownVisual(Core::DrugType type, int stack)
        {
            auto* handler = RE::TESDataHandler::GetSingleton();
            auto* imod    = handler ? handler->LookupForm<RE::TESImageSpaceModifier>(Forms::HighrimImodComedown, Forms::kPluginName) : nullptr;
            if (!imod) {
                spdlog::error("Comedown visual: image space modifier {:06X} not found in {}", Forms::HighrimImodComedown, Forms::kPluginName);
                return;
            }
            const double strength = Core::StrengthForComedown(stack);
            RE::ImageSpaceModifierInstanceForm::Trigger(imod, static_cast<float>(strength), nullptr);
            {
                std::lock_guard lock(g_mutex);
                g_comedownActive = true;
                g_comedownStop   = false;
                g_comedownEnd    = RealSeconds() + Core::kComedownVisualSeconds;
            }
            spdlog::info("Comedown visual: started for {} ({:08X}, stack {}, strength {:.2f}) for {:.0f} s", DrugName(type), imod->GetFormID(), stack,
                strength, Core::kComedownVisualSeconds);
        }

        void StopComedownVisual(const char* why)
        {
            auto* handler = RE::TESDataHandler::GetSingleton();
            auto* imod    = handler ? handler->LookupForm<RE::TESImageSpaceModifier>(Forms::HighrimImodComedown, Forms::kPluginName) : nullptr;
            if (imod) {
                RE::ImageSpaceModifierInstanceForm::Stop(imod);
            }
            spdlog::info("Comedown visual: stopped ({}){}", why, imod ? "" : " - image space modifier not found");
        }

        void OnDrugConsumed(Core::DrugType type)
        {
            const auto settings = Config::Get();
            if (!settings.enabled) {
                spdlog::info("Dose: {} ignored, Highrim is disabled", DrugName(type));
                return;
            }
            const double now = NowHours();
            if (now < 0.0) {
                spdlog::warn("Dose: {} ignored, no calendar", DrugName(type));
                return;
            }
            const auto r = State::RecordDose(type, now, settings.addiction);

            std::lock_guard lock(g_mutex);
            const auto      i             = static_cast<std::size_t>(type);
            const int       windowMin     = static_cast<int>(settings.doseWindowMinutes);
            const double    previousHours = g_totals.lastHours;
            const int       stack         = Core::AddStackedDose(g_stacks, type, now, windowMin);
            const int       total         = Core::AddAnyDose(g_totals, now, windowMin);
            const int       blackoutDoses = static_cast<int>(settings.blackoutDoses);
            const int       level         = Core::StackLevel(stack);
            const double    strength      = type == Core::DrugType::kDepressant ? Core::StrengthForVelvetStack(stack)
                                                                                 : Core::StrengthForStack(stack);
            const auto      tier          = Core::OverdoseRiskTier(total, blackoutDoses);
            const double    chance        = Core::FullBlackoutChance(total, blackoutDoses);
            auto            outcome       = Core::OverdoseOutcome::kNone;
            if (settings.overdose && Core::ShouldBlackout(total, blackoutDoses)) {
                // Every counted dose from the threshold up rolls: full blackout with `chance`, else a faint half the time.
                std::uniform_real_distribution<double> unit(0.0, 1.0);
                const double                           rollA = unit(g_rng);
                const double                           rollB = unit(g_rng);
                outcome                                      = Core::RollOverdose(total, blackoutDoses, rollA, rollB);
                spdlog::info("Overdose roll: total {}/{} chance {:.0f}% -> {} (rolls {:.3f} / {:.3f}; risk tier {})", total, blackoutDoses,
                    chance * 100.0, outcome == Core::OverdoseOutcome::kFull ? "full" : outcome == Core::OverdoseOutcome::kFaint ? "faint" : "none",
                    rollA, rollB, Core::RiskTierName(tier));
                if (outcome == Core::OverdoseOutcome::kFull && !Blackout::FullBlackoutAllowed()) {
                    // bFullBlackout is off (the default): the gear-loss blackout never runs, the milder faint does.
                    spdlog::info("Overdose roll: full -> faint (full blackout is off)");
                    outcome = Core::OverdoseOutcome::kFaint;
                }
            } else if (settings.overdose) {
                spdlog::info("Overdose roll: total {}/{} chance {:.0f}% -> none (below the threshold; risk tier {})", total, blackoutDoses,
                    chance * 100.0, Core::RiskTierName(tier));
            }
            const bool      overdose      = outcome != Core::OverdoseOutcome::kNone;
            std::string     previous      = "none";
            if (previousHours >= 0.0 && std::isfinite(previousHours)) {
                previous = fmt::format("{:.1f} min ago", (now - previousHours) * 60.0);
            }
            const char* verdict = outcome == Core::OverdoseOutcome::kFull    ? "full blackout"
                                  : outcome == Core::OverdoseOutcome::kFaint ? "faint"
                                  : !settings.overdose                       ? "no overdose: bOverdose is off"
                                  : Core::ShouldBlackout(total, blackoutDoses) ? "no overdose: the roll came up empty"
                                                                              : "no overdose: total below the dose count";
            spdlog::info("Dose: {} stack {}/{}, total {}/{}, window {} min, previous dose {} -> {} (level {} strength {:.2f})",
                DrugName(type), stack, blackoutDoses, total, blackoutDoses, windowMin, previous, verdict, level, strength);
            spdlog::info("Dose: {} addiction {:.1f}/{:.1f}/{:.1f} overdose={} (game hour {:.2f}, addiction {}, overdose setting {})",
                DrugName(type), r.after.level[0], r.after.level[1], r.after.level[2], overdose, now,
                settings.addiction ? "on" : "off", settings.overdose ? "on" : "off");

            Core::OnComedownDose(g_comedowns, type, RealSeconds());
            if (type == Core::DrugType::kDepressant) {
                Ragdoll::OnDepressantDose();
            }
            Core::ScheduleDiagnostics(g_diagnostics, type, RealSeconds());
            if (type == Core::DrugType::kHallucinogen && !overdose) {
                g_detectNext = RealSeconds();  // cast on the next tick, then every kDetectLifeRecastSeconds while the high lasts
            }
            if (overdose || g_overdoseDue || g_blackoutDue) {
                // A faint or blackout is coming: no stack ability from here on (and the current one goes).
                g_wantedLevel[i] = 0;
                if (overdose && !g_overdoseDue && !g_blackoutDue) {
                    g_overdoseDue    = RealSeconds();  // knock down now (waits for menus), faint or blackout kRagdollFirstSeconds later
                    g_pendingOutcome = outcome;
                } else if (outcome == Core::OverdoseOutcome::kFull && g_pendingOutcome == Core::OverdoseOutcome::kFaint) {
                    g_pendingOutcome = outcome;  // one already pending and not started yet: a full roll upgrades a pending faint
                    spdlog::info("Overdose roll: the pending faint becomes a full blackout");
                }
            } else {
                g_wantedLevel[i] = level;
            }
            g_strength[i] = strength;
            if (Core::HasHighVisual(type)) {
                g_visualDue[i] = RealSeconds();  // first trigger on the next tick (Rimerush has none)
            }
        }

        class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* event,
                RE::BSTEventSource<RE::TESEquipEvent>*) override
            {
                if (!event || !event->equipped || !event->actor) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player || event->actor.get() != player) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (auto drug = DrugFromForm(event->baseObject)) {
                    spdlog::debug("Dose: equip event for {} (formID {:08X})", DrugName(*drug), event->baseObject);
                    OnDrugConsumed(*drug);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        EquipSink g_sink;
    }

    void Install()
    {
        static std::atomic<bool> done{ false };
        if (done.exchange(true)) {
            return;
        }
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            spdlog::critical("Doses: no script event source holder, doses will not be detected");
            return;
        }
        holder->AddEventSink<RE::TESEquipEvent>(&g_sink);
        spdlog::info("Doses: equip event sink registered");
    }

    void Tick()
    {
        bool                        runOverdose = false;
        bool                        runKnock    = false;
        Core::OverdoseOutcome       knockOutcome = Core::OverdoseOutcome::kNone;
        Core::OverdoseOutcome       runOutcome   = Core::OverdoseOutcome::kNone;
        bool                        stopRiskVisual = false;
        double                      riskVisual     = 0.0;
        bool                        blackoutPending = false;
        std::vector<std::pair<Core::DrugType, int>> due;  // (drug, its stack when the high ended)
        std::vector<Core::DrugType> stops;
        bool                        castDetect = false, checkDetect = false, stopComedown = false;
        std::vector<Core::DiagnosticEntry> diagnostics;
        std::vector<std::pair<Core::DrugType, double>> visuals;
        const bool                  uiBlocked = Blackout::MenuOrPauseActive();
        const bool                  busy      = Blackout::Busy();
        bool                        ragdollActive = false;
        int                         ragdollStack  = 1;
        const auto                  settings      = Config::Get();
        const double                nowHours      = NowHours();
        {
            std::lock_guard lock(g_mutex);
            const double    now = RealSeconds();
            if (g_overdoseDue) {
                // Wait (poll) while a menu is open or the game is paused, e.g. the second dose came from the inventory.
                switch (Core::DecideOverdose(now, *g_overdoseDue, uiBlocked)) {
                case Core::OverdoseAction::kRun:
                    g_overdoseDue.reset();
                    runKnock      = true;
                    knockOutcome  = g_pendingOutcome;
                    g_blackoutDue = Core::BlackoutStartAfterKnock(now);
                    break;
                case Core::OverdoseAction::kDrop:
                    g_overdoseDue.reset();
                    g_pendingOutcome = Core::OverdoseOutcome::kNone;
                    spdlog::warn("Dose: overdose dropped before the knock-down: a menu was open or the game was paused for {:.0f} s",
                        Core::kOverdoseGiveUpSeconds);
                    break;
                default:
                    break;
                }
            }
            if (g_blackoutDue) {
                switch (Core::DecideOverdose(now, *g_blackoutDue, uiBlocked)) {
                case Core::OverdoseAction::kRun:
                    g_blackoutDue.reset();
                    runOverdose      = true;
                    runOutcome       = g_pendingOutcome;
                    g_pendingOutcome = Core::OverdoseOutcome::kNone;
                    break;
                case Core::OverdoseAction::kDrop:
                    g_blackoutDue.reset();
                    g_pendingOutcome = Core::OverdoseOutcome::kNone;
                    spdlog::warn("Dose: overdose dropped before the blackout: a menu was open or the game was paused for {:.0f} s",
                        Core::kOverdoseGiveUpSeconds);
                    break;
                default:
                    break;
                }
            }
            blackoutPending = g_overdoseDue.has_value() || g_blackoutDue.has_value();
            while (auto d = Core::TakeDueComedown(g_comedowns, now)) {
                due.emplace_back(*d, g_stacks.count[static_cast<std::size_t>(*d)]);
                if (*d == Core::DrugType::kHallucinogen) {
                    g_detectNext.reset();  // the high is over: no more Detect Life casts
                }
                // The high ends: its stack ability and screen effect go (the stack count keeps its own window).
                g_wantedLevel[static_cast<std::size_t>(*d)] = 0;
                g_visualDue[static_cast<std::size_t>(*d)].reset();
                g_visualStop[static_cast<std::size_t>(*d)] = true;
            }
            for (std::size_t i = 0; i < 3; ++i) {
                if (g_visualDue[i] && now >= *g_visualDue[i] && !busy && !blackoutPending && !runKnock) {
                    visuals.emplace_back(static_cast<Core::DrugType>(i), g_strength[i]);
                    g_visualDue[i] = now + kVisualPeriodSeconds;
                }
            }
            for (std::size_t i = 0; i < 3; ++i) {
                if (g_visualStop[i]) {
                    g_visualStop[i] = false;
                    if (g_visualActive[i]) {
                        g_visualActive[i] = false;
                        stops.push_back(static_cast<Core::DrugType>(i));
                    }
                }
            }
            // The Velvet Haze high lasts from its dose until its comedown is taken above.
            if (g_detectNext && now >= *g_detectNext && !busy && !blackoutPending && !runKnock) {
                castDetect   = true;
                g_detectNext = now + Core::kDetectLifeRecastSeconds;
            }
            if (g_detectCheckAt && now >= *g_detectCheckAt) {
                g_detectCheckAt.reset();
                checkDetect = true;
            }
            if (g_comedownStop || (g_comedownEnd && now >= *g_comedownEnd)) {
                if (g_comedownActive) {
                    stopComedown = true;
                }
                g_comedownActive = false;
                g_comedownStop   = false;
                g_comedownEnd.reset();
            }
            g_shakyWanted = Core::IsShakyHigh(g_comedowns);
            {
                // Overdose risk indicator: the tier of the dose total while it is still inside the window.
                auto tier = Core::RiskTier::kNone;
                if (settings.enabled && settings.overdose && nowHours >= 0.0) {
                    tier = Core::OverdoseRiskTier(
                        Core::ActiveDoseTotal(g_totals, nowHours, static_cast<int>(settings.doseWindowMinutes)), static_cast<int>(settings.blackoutDoses));
                }
                g_riskWanted        = static_cast<int>(tier);
                const double strength = Core::RiskVisualStrength(tier);
                if (strength > 0.0) {
                    if (strength != g_riskStrength) {
                        g_riskStrength  = strength;  // a new tier: trigger the warning at its strength right away
                        g_riskVisualDue = now;
                    } else if (!g_riskVisualDue) {
                        g_riskVisualDue = now;
                    }
                    if (g_riskVisualDue && now >= *g_riskVisualDue && !busy && !blackoutPending && !runKnock) {
                        riskVisual      = strength;
                        g_riskVisualDue = now + kVisualPeriodSeconds;
                    }
                } else {
                    g_riskStrength = 0.0;
                    g_riskVisualDue.reset();
                    if (g_riskVisualActive) {
                        g_riskVisualActive = false;
                        stopRiskVisual     = true;
                    }
                }
            }
            ragdollActive = g_comedowns.dueSeconds[static_cast<std::size_t>(Core::DrugType::kDepressant)].has_value();
            ragdollStack  = g_stacks.count[static_cast<std::size_t>(Core::DrugType::kDepressant)];
            while (auto e = Core::TakeDueDiagnostic(g_diagnostics, now)) {
                diagnostics.push_back(*e);
            }
        }
        for (auto d : stops) {
            StopVisual(d);
        }
        if (stopComedown) {
            StopComedownVisual("the comedown ended or the game state was reset");
        }
        if (stopRiskVisual) {
            StopRiskVisual();
        }
        for (const auto& [d, stack] : due) {
            CastComedown(d);
            RE::SendHUDMessage::ShowHUDMessage(Core::ComedownMessage(d).c_str(), nullptr, false);
            Diag::Schedule(ComedownLocalId(d), "comedown");
            StartComedownVisual(d, stack);
        }
        if (castDetect) {
            CastDetectLife();
        }
        if (checkDetect) {
            LogDetectLifeCheck();
        }
        if (runKnock) {
            const bool  faint = knockOutcome == Core::OverdoseOutcome::kFaint;
            const char* route = Ragdoll::KnockNow(faint ? "overdose faint" : "overdose");
            spdlog::info("{}: ragdoll first (route {}), starting in {:.1f} s", faint ? "Faint" : "Blackout",
                route ? route : "none (it starts anyway)", Core::kRagdollFirstSeconds);
        }
        for (const auto& [type, strength] : visuals) {
            TriggerVisual(type, strength);
        }
        if (riskVisual > 0.0) {
            TriggerRiskVisual(riskVisual);
        }
        SyncStackAbilities();
        SyncShaky();
        SyncRiskAbility();
        Ragdoll::Tick(ragdollActive && !runOverdose && !blackoutPending && !runKnock, ragdollStack);
        for (const auto& e : diagnostics) {
            LogActiveEffects(e.type, e.pass);
        }
        if (runOverdose) {
            if (runOutcome == Core::OverdoseOutcome::kFull && !Blackout::FullBlackoutAllowed()) {
                spdlog::info("Overdose roll: full -> faint (full blackout is off)");  // the setting changed while the knock-down ran
                runOutcome = Core::OverdoseOutcome::kFaint;
            }
            if (runOutcome == Core::OverdoseOutcome::kFull) {
                spdlog::info("Dose: overdose, starting the blackout");
                Blackout::Run("overdose");
            } else {
                spdlog::info("Dose: overdose, starting the faint");
                Blackout::RunFaint("overdose");
            }
        }
    }

    void CancelComedowns()
    {
        std::lock_guard lock(g_mutex);
        Core::CancelComedowns(g_comedowns);
        g_diagnostics.pending.clear();
        g_overdoseDue.reset();
        g_blackoutDue.reset();
        g_pendingOutcome = Core::OverdoseOutcome::kNone;
        // Load, new game, revert or blackout start: forget the stacks and have the game thread strip every stack
        // ability (unknown applied state forces a full sync).
        Core::ResetStacks(g_stacks);
        g_totals = Core::DoseTotals{};
        g_wantedLevel  = {};
        g_appliedLevel = { -1, -1, -1 };
        // The overdose risk indicator goes with the total: the game thread strips the ability (unknown applied state) and
        // stops a running warning (Tick sees tier none).
        g_riskWanted    = 0;
        g_riskApplied   = -1;
        g_riskStrength  = 0.0;
        g_riskVisualDue.reset();
        g_shakyWanted  = false;
        g_shakyApplied = -1;  // unknown: the game thread strips the ability on its next tick
        g_visualDue    = {};
        g_visualStop   = g_visualActive;  // the game thread stops whatever screen effect is still running
        g_detectNext.reset();
        g_detectCheckAt.reset();
        g_comedownStop = g_comedownActive;  // the black-and-white comedown look goes too
        g_comedownEnd.reset();
        ++g_epoch;
        Ragdoll::Reset();
    }
}
