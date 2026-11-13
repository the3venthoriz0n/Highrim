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
        {
            std::lock_guard lock(g_mutex);
            const double    now = RealSeconds();
            if (g_overdoseDue && now >= *g_overdoseDue) {
                g_overdoseDue.reset();
                runOverdose = true;
            }
            while (auto d = Core::TakeDueComedown(g_comedowns, now)) {
                due.push_back(*d);
            }
        }
        for (auto d : due) {
            CastComedown(d);
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
        g_overdoseDue.reset();
    }
}
