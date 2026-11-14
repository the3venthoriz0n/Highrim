#include "PCH.h"

#include "Withdrawal.h"

#include "Blackout.h"
#include "Config.h"
#include "Core.h"
#include "Diag.h"
#include "Forms.h"
#include "Ragdoll.h"
#include "State.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
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

        // --- Rimerush withdrawal drain (game thread only) ---------------------------------------------------------
        constexpr double kDrainPollSeconds = 1.0;
        constexpr double kDrainMaxStepSeconds = 3.0;   // a long gap (menu, load) never drains more than this at once
        constexpr double kDrainGiveUpSeconds  = 30.0;  // a blackout that cannot start (combat, controls) is dropped after this
        constexpr std::array<RE::ActorValue, 3> kBars{ RE::ActorValue::kHealth, RE::ActorValue::kMagicka, RE::ActorValue::kStamina };
        constexpr std::array<const char*, 3>    kBarNames{ "Health", "Magicka", "Stamina" };
        bool                  g_drainActive = false;
        int                   g_drainStage  = 0;
        double                g_drainNext   = 0.0;
        double                g_drainLast   = 0.0;
        double                g_pauseUntil  = 0.0;
        std::array<double, 3> g_lastSet{ std::nan(""), std::nan(""), std::nan("") };
        bool                  g_depleted    = false;         // depletion already handled in this drain
        std::optional<double> g_blackoutDue;                 // real seconds: the blackout starts then (after the knock)
        double                g_blackoutDueFirst = 0.0;      // when the depletion happened (for the give-up time)
        std::uint32_t         g_blackoutGen = 0;
        bool                  g_awaitRestore = false;        // the blackout was started: restore the bars when it ends
        bool                  g_blackoutSeen = false;

        void ResetDrain(const char* why)
        {
            if (g_drainActive || g_blackoutDue || g_awaitRestore) spdlog::info("Rimerush drain: reset ({})", why);
            g_drainActive = false;
            g_drainStage  = 0;
            g_depleted    = false;
            g_lastSet     = { std::nan(""), std::nan(""), std::nan("") };
            g_blackoutDue.reset();
            g_awaitRestore = false;
            g_blackoutSeen = false;
        }
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

    // Bars after a Rimerush withdrawal blackout: each is raised to kDrainBlackoutRestoreFraction of its maximum (never lowered).
    static void RestoreBars(RE::PlayerCharacter* player)
    {
        auto* owner = player->AsActorValueOwner();
        if (!owner) return;
        for (std::size_t i = 0; i < 3; ++i) {
            const double max     = player->GetActorValueMax(kBars[i]);
            const double current = owner->GetActorValue(kBars[i]);
            const double target  = max * Core::kDrainBlackoutRestoreFraction;
            if (current < target) owner->RestoreActorValue(kBars[i], static_cast<float>(target - current));
            spdlog::info("Rimerush drain: {} restored {:.1f} -> {:.1f} (max {:.1f})", kBarNames[i], current, std::max(current, target), max);
        }
    }

    // Once per real second. Drains Health, Magicka and Stamina while the Rimerush withdrawal condition holds (see Core.h).
    static void DrainTick(double now)
    {
        if (now < g_drainNext) return;
        g_drainNext = now + kDrainPollSeconds;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui     = RE::UI::GetSingleton();
        if (!player || !player->Is3DLoaded() || !player->GetParentCell() || !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
            ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
            return;
        }
        const auto settings = Config::Get();

        // A blackout we started: nothing drains while it runs; when it ends the bars are restored and the drain pauses.
        if (g_awaitRestore) {
            if (Blackout::Busy()) {
                g_blackoutSeen = true;
                g_drainLast    = now;
                return;
            }
            if (g_blackoutSeen) {
                const bool sameGame = Blackout::Generation() == g_blackoutGen;
                ResetDrain("blackout ended");
                if (sameGame && !player->IsDead()) {
                    RestoreBars(player);
                    g_pauseUntil = now + Core::kDrainPauseSeconds;
                    spdlog::info("Rimerush drain: blackout over, bars restored to 25%, drain paused for {:.0f} s", Core::kDrainPauseSeconds);
                }
                return;
            }
            // Run() was requested but never started (a guard refused it): fall through to the due-time handling below.
            g_awaitRestore = false;
        }

        const bool on = settings.enabled && settings.addiction;
        bool       active = false;
        double     level = 0.0, intoWithdrawal = 0.0, since = 0.0;
        if (on && !player->IsDead()) {
            const double hours = NowHours();
            if (hours < 0.0) return;
            auto snap = State::Snapshot();
            Core::Decay(snap, hours);
            level = snap.level[0];
            since = snap.lastDoseHours[0] < 0.0 ? 1e9 : hours - snap.lastDoseHours[0];
            intoWithdrawal = since - Core::kWithdrawalGraceHours;
            active         = Core::RimerushDrainActive(static_cast<float>(level), since) && now >= g_pauseUntil;
        }

        if (!active) {
            if (g_drainActive) {
                const bool dose = on && since < Core::kWithdrawalGraceHours;
                spdlog::info("Rimerush drain: stopped ({})", dose ? "a Rimerush dose was taken" : "no longer in withdrawal or paused or off");
                if (dose) RE::SendHUDMessage::ShowHUDMessage(Core::RimerushDrainStopMessage(), nullptr, false);
            }
            ResetDrain("not active");
            return;
        }
        if (UiBlocksPulse()) {  // a menu is open or the game is paused: no drain, and the clock does not run
            g_drainLast = now;
            return;
        }

        auto* owner = player->AsActorValueOwner();
        if (!owner) return;
        if (!g_drainActive) {
            g_drainActive = true;
            g_drainStage  = 0;
            g_drainLast   = now;
            g_depleted    = false;
            g_lastSet     = { std::nan(""), std::nan(""), std::nan("") };
            spdlog::info("Rimerush drain: started (level {:.1f}, {:.1f} h into withdrawal)", level, intoWithdrawal);
            RE::SendHUDMessage::ShowHUDMessage(Core::RimerushDrainStartMessage(), nullptr, false);
        }

        const double dt   = std::clamp(now - g_drainLast, 0.0, kDrainMaxStepSeconds);
        g_drainLast       = now;
        const double rate = Core::RimerushDrainPerSecond(level, intoWithdrawal);
        std::array<double, 3> after{};
        double                lowest = 1.0;
        for (std::size_t i = 0; i < 3; ++i) {
            const double current = owner->GetActorValue(kBars[i]);
            const double base    = Core::DrainBaseValue(current, g_lastSet[i], Core::kRegenRiseLimit);  // cancels natural regeneration
            const double floor   = i == 0 ? Core::kHealthDrainFloor : 0.0;
            const double target  = Core::DrainedValue(base, rate, dt, floor);
            if (target < current) owner->DamageActorValue(kBars[i], static_cast<float>(current - target));
            after[i]     = std::min(current, target);
            g_lastSet[i] = after[i];
            const double max = player->GetActorValueMax(kBars[i]);
            if (max > 0.0) lowest = std::min(lowest, after[i] / max);
        }
        const int stage = Core::DrainWarningStage(lowest);
        if (stage > g_drainStage) {
            g_drainStage = stage;
            const char* text = stage == 1 ? "Rimerush withdrawal: your reserves are half gone. You need more Rimerush."
                                          : "Rimerush withdrawal: you are almost out. You need more Rimerush now.";
            RE::SendHUDMessage::ShowHUDMessage(text, nullptr, false);
            spdlog::info("Rimerush drain: warning stage {} (lowest bar {:.0f}% of its maximum; H {:.1f} M {:.1f} S {:.1f}; {:.2f}/s per bar)",
                stage, lowest * 100.0, after[0], after[1], after[2], rate);
        }

        if (!Core::DrainDepleted(after[0], after[1], after[2])) return;

        // A bar is empty. The full blackout is the only consequence, and only when both switches allow it.
        if (!g_depleted) {
            g_depleted = true;
            spdlog::info("Rimerush drain: a bar is depleted (H {:.1f} M {:.1f} S {:.1f})", after[0], after[1], after[2]);
            if (!Blackout::FullBlackoutAllowed()) {
                spdlog::info("Rimerush withdrawal: bars at the floor, full blackout is off");
            } else if (!settings.rimerushWithdrawalBlackout) {
                spdlog::info("Rimerush withdrawal: bars at the floor, bRimerushWithdrawalBlackout is off");
            } else {
                const char* route = Ragdoll::KnockNow("rimerush withdrawal");
                spdlog::info("Rimerush drain: ragdoll first (route {}), blackout in {:.1f} s", route ? route : "none (blackout starts anyway)",
                    Core::kRagdollFirstSeconds);
                g_blackoutDue      = Core::BlackoutStartAfterKnock(now);
                g_blackoutDueFirst = now;
                g_blackoutGen      = Blackout::Generation();
            }
        }
    }

    // Starts a due withdrawal blackout through the same entry point as the overdose (Blackout::Run applies every guard).
    static void BlackoutTick(double now)
    {
        if (!g_blackoutDue) return;
        if (Blackout::Generation() != g_blackoutGen) {
            spdlog::info("Rimerush drain: blackout dropped, a load, new game or death happened meanwhile");
            ResetDrain("generation changed");
            return;
        }
        if (now < *g_blackoutDue) return;
        if (Blackout::MenuOrPauseActive()) {  // deferral: wait for the menu to close (give up like the overdose does)
            if (now - *g_blackoutDue > Core::kOverdoseGiveUpSeconds) {
                spdlog::warn("Rimerush drain: blackout dropped, a menu was open for {:.0f} s", Core::kOverdoseGiveUpSeconds);
                g_blackoutDue.reset();
            }
            return;
        }
        if (!Blackout::FullBlackoutAllowed() || !Config::Get().rimerushWithdrawalBlackout) {
            spdlog::info("Rimerush withdrawal: blackout cancelled, a switch was turned off meanwhile");
            g_blackoutDue.reset();
            return;
        }
        spdlog::info("Rimerush drain: starting the blackout (reason: rimerush withdrawal)");
        Blackout::Run("rimerush withdrawal");
        if (Blackout::Busy()) {
            g_blackoutDue.reset();
            g_awaitRestore = true;
            g_blackoutSeen = true;
        } else if (now - g_blackoutDueFirst > kDrainGiveUpSeconds + Core::kRagdollFirstSeconds) {
            spdlog::warn("Rimerush drain: the blackout could not start for {:.0f} s (a guard refused it), dropped", kDrainGiveUpSeconds);
            g_blackoutDue.reset();
        } else {
            *g_blackoutDue = now + 1.0;  // retried next poll
        }
    }

    void OnGameLoaded()
    {
        ResetDrain("game loaded");
        g_pauseUntil = 0.0;
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
            DrainTick(now);
            BlackoutTick(now);
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
