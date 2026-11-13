#include "PCH.h"

#include "Doses.h"

#include "Blackout.h"
#include "Config.h"
#include "Core.h"
#include "Forms.h"
#include "State.h"

#include <vector>

namespace Highrim::Doses
{
    namespace
    {
        // Give the potion's own effect time to apply before the blackout takes the player's gear.
        constexpr double kOverdoseDelaySeconds = 1.5;

        std::mutex            g_mutex;
        Core::ComedownTimers  g_comedowns{};
        Core::DiagnosticTimers g_diagnostics{};
        std::optional<double> g_overdoseDue;  // real seconds

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
            int active = 0;
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
                                break;
                            }
                        }
                    }
                }
            } else {
                spdlog::warn("  no active effect list on the player");
            }
            const int expected = potion ? static_cast<int>(potion->effects.size()) : 0;
            spdlog::info("Highrim effects active: {} of {} expected for {}", active, expected, DrugName(type));
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
            const auto r = State::RecordDose(type, now, static_cast<int>(settings.doseWindowMinutes), settings.addiction);
            spdlog::info("Dose: {} addiction {:.1f}/{:.1f}/{:.1f} overdose={} (game hour {:.2f}, addiction {}, overdose setting {})",
                DrugName(type), r.after.level[0], r.after.level[1], r.after.level[2], r.overdose, now,
                settings.addiction ? "on" : "off", settings.overdose ? "on" : "off");

            std::lock_guard lock(g_mutex);
            Core::OnComedownDose(g_comedowns, type, RealSeconds());
            Core::ScheduleDiagnostics(g_diagnostics, type, RealSeconds());
            if (r.overdose && settings.overdose && !g_overdoseDue) {
                g_overdoseDue = RealSeconds() + kOverdoseDelaySeconds;
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
        std::vector<Core::DrugType> due;
        std::vector<Core::DiagnosticEntry> diagnostics;
        const bool                  uiBlocked = Blackout::MenuOrPauseActive();
        {
            std::lock_guard lock(g_mutex);
            const double    now = RealSeconds();
            if (g_overdoseDue) {
                // Wait (poll) while a menu is open or the game is paused, e.g. the second dose came from the inventory.
                switch (Core::DecideOverdose(now, *g_overdoseDue, uiBlocked)) {
                case Core::OverdoseAction::kRun:
                    g_overdoseDue.reset();
                    runOverdose = true;
                    break;
                case Core::OverdoseAction::kDrop:
                    g_overdoseDue.reset();
                    spdlog::warn("Dose: overdose dropped, a menu was open or the game was paused for {:.0f} s", Core::kOverdoseGiveUpSeconds);
                    break;
                default:
                    break;
                }
            }
            while (auto d = Core::TakeDueComedown(g_comedowns, now)) {
                due.push_back(*d);
            }
            while (auto e = Core::TakeDueDiagnostic(g_diagnostics, now)) {
                diagnostics.push_back(*e);
            }
        }
        for (auto d : due) {
            CastComedown(d);
        }
        for (const auto& e : diagnostics) {
            LogActiveEffects(e.type, e.pass);
        }
        if (runOverdose) {
            spdlog::info("Dose: overdose, starting the blackout");
            Blackout::Run("overdose");
        }
    }

    void CancelComedowns()
    {
        std::lock_guard lock(g_mutex);
        Core::CancelComedowns(g_comedowns);
        g_diagnostics.pending.clear();
        g_overdoseDue.reset();
    }
}
