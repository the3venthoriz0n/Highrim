#include "PCH.h"

#include "Withdrawal.h"

#include "Blackout.h"
#include "Config.h"
#include "Core.h"
#include "Diag.h"
#include "Forms.h"
#include "State.h"

#include <array>
#include <random>

namespace Highrim::Withdrawal
{
    namespace
    {
        constexpr double kPollSeconds = 5.0;

        constexpr std::array<std::uint32_t, 3> kAbilityIds{
            Forms::HighrimWithdrawal1, Forms::HighrimWithdrawal2, Forms::HighrimWithdrawal3
        };

        constexpr std::array<std::uint32_t, 3> kAddictedIds{
            Forms::HighrimAddictedRimerush, Forms::HighrimAddictedVelvetHaze, Forms::HighrimAddictedDreamcap
        };

        // Game thread only. -1 = unknown (re-apply on the next tick).
        int                   g_applied  = -1;
        std::array<int, 3>    g_addicted{ -1, -1, -1 };  // 1 = the player carries "Addicted: <drug>", 0 = not, -1 = unknown
        std::optional<Core::Addiction> g_prev;           // the addiction state at the previous poll (to see doses)
        double                g_nextPoll = 0.0;
        std::optional<double> g_blurDue;

        double RealSeconds()
        {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        double NowHours()
        {
            auto* calendar = RE::Calendar::GetSingleton();
            if (!calendar || !calendar->gameDaysPassed) return -1.0;
            return static_cast<double>(calendar->gameDaysPassed->value) * 24.0;
        }

        RE::SpellItem* Lookup(std::uint32_t localId)
        {
            auto* handler = RE::TESDataHandler::GetSingleton();
            return handler ? handler->LookupForm<RE::SpellItem>(localId, Forms::kPluginName) : nullptr;
        }

        // Removes every Highrim withdrawal ability from the player. Returns false if a spell is missing.
        bool RemoveAll(RE::PlayerCharacter* player)
        {
            bool ok = true;
            for (std::size_t i = 0; i < kAbilityIds.size(); ++i) {
                auto* spell = Lookup(kAbilityIds[i]);
                if (!spell) {
                    spdlog::error("Withdrawal: spell for tier {} not found in {}", i + 1, Forms::kPluginName);
                    ok = false;
                    continue;
                }
                if (player->HasSpell(spell)) {
                    const bool removed = player->RemoveSpell(spell);
                    spdlog::info("Withdrawal: removed tier {} ability, RemoveSpell -> {}", i + 1, removed);
                }
            }
            return ok;
        }

        // Makes the player carry exactly the ability for `tier` (none for 0). Sets g_applied only on success.
        // `notice` is the HUD text for a change of tier ("" = none); it is shown only when the change succeeded.
        void Apply(RE::PlayerCharacter* player, int tier, const char* why, const std::string& notice)
        {
            const auto plan = Core::PlanWithdrawal(g_applied, tier);
            if (!plan.change) return;
            const int before = g_applied;
            if (!RemoveAll(player)) return;  // retried next tick
            if (plan.addTier > 0) {
                auto* spell = Lookup(kAbilityIds[static_cast<std::size_t>(plan.addTier - 1)]);
                if (!spell) return;
                const bool added = player->AddSpell(spell);
                spdlog::info("Withdrawal: tier {} -> {} ({}), AddSpell -> {}", before, plan.addTier, why, added);
                if (!added && !player->HasSpell(spell)) return;  // retried next tick
                Diag::Schedule(kAbilityIds[static_cast<std::size_t>(plan.addTier - 1)], fmt::format("withdrawal tier {}", plan.addTier));
            } else {
                spdlog::info("Withdrawal: tier {} -> 0 ({})", before, why);
            }
            // The HUD message tells the player about a change, not about re-applying the same tier after a load
            // (before = -1 then); the text is decided by Core::ClassifyWithdrawalChange.
            if (before != plan.addTier && before >= 0 && !notice.empty()) {
                RE::SendHUDMessage::ShowHUDMessage(notice.c_str(), nullptr, false);
                spdlog::info("Withdrawal: HUD message: {}", notice);
            }
            g_applied = plan.addTier;
        }

        // Makes the player carry "Addicted: <drug>" exactly for the drugs in `want`, never twice. A drug whose flag was
        // known to be on and goes off because its dependency fell says so on the HUD (only with addiction enabled).
        void SyncAddicted(RE::PlayerCharacter* player, const std::array<bool, 3>& want, bool announce)
        {
            for (std::size_t i = 0; i < 3; ++i) {
                const int wanted = want[i] ? 1 : 0;
                if (g_addicted[i] == wanted) continue;
                const auto type  = static_cast<Core::DrugType>(i);
                auto*      spell = Lookup(kAddictedIds[i]);
                if (!spell) {
                    spdlog::error("Addicted: ability for {} not found in {}", Core::DrugDisplayName(type), Forms::kPluginName);
                    continue;  // retried next tick
                }
                const bool has = player->HasSpell(spell);
                if (wanted == 1) {
                    if (!has) {
                        const bool added = player->AddSpell(spell);
                        spdlog::info("Addicted: {} ability added, AddSpell -> {}", Core::DrugDisplayName(type), added);
                        if (!added && !player->HasSpell(spell)) continue;
                    }
                } else if (has) {
                    const bool removed = player->RemoveSpell(spell);
                    spdlog::info("Addicted: {} ability removed, RemoveSpell -> {}", Core::DrugDisplayName(type), removed);
                    if (!removed && player->HasSpell(spell)) continue;
                }
                const int before = g_addicted[i];
                g_addicted[i]    = wanted;
                if (before == 1 && wanted == 0 && announce) {
                    const auto text = Core::NoLongerAddictedMessage(type);
                    RE::SendHUDMessage::ShowHUDMessage(text.c_str(), nullptr, false);
                    spdlog::info("Addicted: HUD message: {}", text);
                }
            }
        }

        bool UiBlocksPulse()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return true;
            return ui->GameIsPaused() || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) ||
                   ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
                   ui->IsMenuOpen(RE::Console::MENU_NAME);
        }

        void CastBlurPulse(RE::PlayerCharacter* player)
        {
            auto* spell = Lookup(Forms::HighrimBlurPulse);
            if (!spell) {
                spdlog::error("Withdrawal: blur pulse spell not found in {}", Forms::kPluginName);
                return;
            }
            auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            if (!caster) {
                spdlog::error("Withdrawal: no instant caster on the player, blur pulse skipped");
                return;
            }
            caster->CastSpellImmediate(spell, false, player, 1.0f, false, 0.0f, nullptr);
            spdlog::info("Withdrawal: cast blur pulse");
        }
    }

    void OnGameLoaded()
    {
        g_applied  = -1;
        g_addicted = { -1, -1, -1 };
        g_prev.reset();
        g_nextPoll = 0.0;
        g_blurDue.reset();
    }

    void Tick()
    {
        try {
            const double now = RealSeconds();
            if (now < g_nextPoll) return;
            g_nextPoll = now + kPollSeconds;

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* ui     = RE::UI::GetSingleton();
            if (!player || !player->Is3DLoaded() || !player->GetParentCell() || !ui ||
                ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                return;
            }

            const auto settings = Config::Get();
            int        tier     = 0;
            std::array<bool, 3> addicted{};
            const bool          addictionOn = settings.addiction && settings.enabled;
            if (!addictionOn) {
                // Switched off: strip the withdrawal abilities, once (g_applied 0 means nothing is applied).
                if (g_applied != 0) {
                    spdlog::info("Withdrawal: addiction/mod off, removing abilities");
                    if (RemoveAll(player)) g_applied = 0;
                }
                g_prev.reset();
            } else {
                const double hours = NowHours();
                if (hours < 0.0) return;
                const auto snap   = State::Snapshot();
                tier              = Core::HighestTier(snap, hours);
                addicted          = Core::AddictedFlags(snap, hours);
                const auto worst  = Core::WorstDrugForTier(snap, hours);
                const bool dose   = g_prev && Core::DoseSince(*g_prev, snap);
                g_prev            = snap;
                std::string notice;
                switch (Core::ClassifyWithdrawalChange(g_applied, tier, dose)) {
                case Core::WithdrawalNotice::kRaised:
                    if (worst) notice = Core::WithdrawalMessage(tier, *worst);
                    break;
                case Core::WithdrawalNotice::kEased:
                    notice = Core::CravingsEaseMessage();
                    break;
                default:
                    break;
                }
                if (g_applied != tier) {
                    spdlog::info("Withdrawal: tier {} -> {}, worst drug {}, dose since last poll {}", g_applied, tier,
                        worst ? Core::DrugDisplayName(*worst) : "none", dose);
                }
                Apply(player, tier, "poll", notice);
            }
            SyncAddicted(player, addicted, addictionOn);

            // Blur pulse: the hangover ability itself is owned by Blackout; we only look at it. The hangover belongs
            // to the overdose feature, so it pulses even with addiction off (tier stays 0 then, so only the hangover
            // makes it eligible). With the whole mod off nothing pulses.
            bool hangover = false;
            if (settings.enabled) {
                if (auto* h = Lookup(Forms::HighrimHangover)) hangover = player->HasSpell(h);
            }
            static std::mt19937 rng{ std::random_device{}() };
            const double        r       = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
            const bool          blocked = UiBlocksPulse() || Blackout::Busy() || player->IsDead();
            // While blocked the schedule is not advanced: a due pulse fires once the menu closes.
            if (!blocked && Core::TickBlurPulse(g_blurDue, now, tier, hangover, r)) {
                CastBlurPulse(player);
            }
        } catch (const std::exception& e) {
            spdlog::error("Withdrawal: exception in tick: {}", e.what());
        } catch (...) {
            spdlog::error("Withdrawal: unknown exception in tick");
        }
    }
}
