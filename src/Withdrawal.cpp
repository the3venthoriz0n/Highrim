#include "PCH.h"

#include "Withdrawal.h"

#include "Blackout.h"
#include "Config.h"
#include "Core.h"
#include "Diag.h"
#include "Forms.h"
#include "Ragdoll.h"
#include "State.h"
#include "Timed.h"

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

        // Ravage drain (Core::kDrainUsesRavage): the HighrimRavage spell lowers the MAXIMUM of each bar (a peak value modifier,
        // as Survival Mode's penalties do), which the HUD should show as the red part of the bar (unverified in game).
        // g_ravageWanted is the amount (points off the maximum) the drain has reached; g_ravageApplied what the live spell
        // carries (negative = unknown: the next cast clears whatever is live first).
        std::array<double, 3> g_ravageWanted{ 0.0, 0.0, 0.0 };
        std::array<double, 3> g_ravageApplied{ -1.0, -1.0, -1.0 };
        std::optional<double> g_restoreAt;                   // real seconds: restore the bars after the ravage is gone (blackout end)

        void ResetDrain(const char* why)
        {
            if (g_drainActive || g_blackoutDue || g_awaitRestore) spdlog::info("Rimerush drain: reset ({})", why);
            g_drainActive = false;
            g_drainStage  = 0;
            g_depleted    = false;
            g_lastSet     = { std::nan(""), std::nan(""), std::nan("") };
            g_ravageWanted = { 0.0, 0.0, 0.0 };
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

        // --- Task 18: timed "Addicted" and withdrawal spells ---------------------------------------------------------
        // The old lasting abilities (kAbilityIds / kAddictedIds) are only ever removed now (saves made before still carry
        // them). The effects the player gets are timed spells whose length is the time until the drug's dependency falls
        // below Core::kTier1Level, in real seconds (Core::SecondsUntilClear with the game's timescale). The record holds a
        // placeholder duration; after the cast the live ActiveEffects get their real one (elapsed + time left, every effect
        // of the spell: the hidden per-skill ones have their own timers too). If that cannot be done (no live effect shows up
        // within kTimedFixGiveUpSeconds), g_useVariants switches to the fixed-length variants for the rest of the session.
        constexpr double kTimedFixPollSeconds   = 0.5;   // a pending duration fix is tried this often (cheap: a few effects)
        constexpr double kTimedFixGiveUpSeconds = 10.0;  // no live effect after this long: fall back to the variants
        constexpr double kTimedMinWantedSeconds = 1.0;

        bool g_useVariants = false;  // game thread only; the fallback is chosen at runtime, never saved

        struct TimedSlot
        {
            int    idx{ -1 };          // spell cast: 0 = base spell (live duration set at runtime), 1.. = fixed-length variant; -1 = none or unknown
            double lastSync{ -1.0 };   // real seconds of the last duration set (or the cast)
            double castAt{ -1.0 };
            double wantedAtCast{ 0.0 };
            bool   pendingFix{ false };
        };
        std::array<TimedSlot, 3> g_addTimed{};  // one per drug
        TimedSlot                g_wdTimed{};   // the withdrawal spell of the applied tier

        using IdOf = std::uint32_t (*)(int slot, int idx);
        std::uint32_t AddictedId(int drug, int idx) { return Forms::kTimedAddicted[drug][idx]; }
        std::uint32_t WithdrawalId(int tier, int idx) { return Forms::kTimedWithdrawal[tier - 1][idx]; }  // tier 1..3

        double Timescale()
        {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar && calendar->timeScale ? static_cast<double>(calendar->timeScale->value) : 20.0;
        }

        void DispelSlot(IdOf idOf, int slot)
        {
            for (int idx = 0; idx <= Forms::kTimedVariantCount; ++idx) {
                if (auto* spell = Timed::Lookup(idOf(slot, idx))) {
                    const int n = Timed::Dispel(spell);
                    if (n > 0) spdlog::info("Timed: dispelled {} effect(s) of '{}'", n, spell->GetName());
                }
            }
        }

        // Makes sure the slot's spell is on the player with about `wantedSeconds` left. Returns true when it is (or was just
        // cast and its effects are about to appear). Real-seconds clock `now`; doseSeen asks for a re-sync right away.
        bool EnsureTimed(TimedSlot& slot, IdOf idOf, int slotKey, double wantedSeconds, double now, bool doseSeen, const std::string& label)
        {
            wantedSeconds = std::max(kTimedMinWantedSeconds, wantedSeconds);
            if (slot.idx >= 0) {
                auto*      current = Timed::Lookup(idOf(slotKey, slot.idx));
                const bool live    = current && Timed::CountLive(current) > 0;
                if (!live) {
                    if (now - slot.castAt < kTimedFixGiveUpSeconds) return true;  // just cast: its effects show up a moment later
                    spdlog::warn("{}: timed spell is not on the player any more, cast again", label);
                    slot.idx = -1;
                } else if (slot.idx == 0) {
                    if (!slot.pendingFix && Core::TimedResyncDue(now, slot.lastSync, doseSeen)) {
                        const int n = Timed::SetRemaining(current, wantedSeconds);
                        slot.lastSync = now;
                        spdlog::info("{} timed {:.0f} s left (re-sync, {} effects set, read back {:.0f} s{})", label, wantedSeconds, n,
                            Timed::RemainingSeconds(current), doseSeen ? ", after a dose" : "");
                    }
                    return true;
                } else {
                    // A fixed-length variant: re-checked once per minute (or after a dose); recast when another one fits better.
                    if (!Core::TimedResyncDue(now, slot.lastSync, doseSeen)) return true;
                    slot.lastSync     = now;
                    const int wantIdx = 1 + static_cast<int>(Core::PickTimedVariant(wantedSeconds));
                    if (wantIdx == slot.idx) return true;
                    spdlog::info("{}: variant {} -> {} for {:.0f} s left", label, slot.idx - 1, wantIdx - 1, wantedSeconds);
                    slot.idx = -1;
                }
            }
            // (Re)cast: clear anything of this slot first (an old variant, or effects that came back with a save).
            DispelSlot(idOf, slotKey);
            const int wantIdx = g_useVariants ? 1 + static_cast<int>(Core::PickTimedVariant(wantedSeconds)) : 0;
            auto*     spell   = Timed::Lookup(idOf(slotKey, wantIdx));
            if (!spell) {
                spdlog::error("{}: timed spell {:06X} not found in {}", label, idOf(slotKey, wantIdx), Forms::kPluginName);
                return false;
            }
            if (!Timed::Cast(spell)) return false;
            slot.idx          = wantIdx;
            slot.castAt       = now;
            slot.lastSync     = now;
            slot.wantedAtCast = wantedSeconds;
            slot.pendingFix   = wantIdx == 0;
            spdlog::info("{} timed spell cast ('{}' {:08X}), {:.0f} s wanted{}", label, spell->GetName(), spell->GetFormID(), wantedSeconds,
                wantIdx == 0 ? "" : fmt::format(" (fixed-length variant {}, fallback)", wantIdx - 1));
            return true;
        }

        // Every kTimedFixPollSeconds: sets the live duration of a spell that was just cast (its effects need a moment to
        // appear). Gives up after kTimedFixGiveUpSeconds and switches to the fixed-length variants.
        void FixPending(TimedSlot& slot, IdOf idOf, int slotKey, double now, const std::string& label)
        {
            if (!slot.pendingFix || slot.idx != 0) return;
            auto*        spell     = Timed::Lookup(idOf(slotKey, 0));
            const double remaining = std::max(kTimedMinWantedSeconds, slot.wantedAtCast - (now - slot.castAt));
            const int    n         = spell ? Timed::SetRemaining(spell, remaining) : 0;
            if (n > 0) {
                slot.pendingFix = false;
                slot.lastSync   = now;
                spdlog::info("{} timed {:.0f} s left ({} effects set, read back {:.0f} s)", label, remaining, n, Timed::RemainingSeconds(spell));
            } else if (now - slot.castAt > kTimedFixGiveUpSeconds) {
                g_useVariants   = true;
                slot.pendingFix = false;
                slot.idx        = -1;  // cast again, as a fixed-length variant, at the next poll
                spdlog::error("{}: no live effect {:.0f} s after the cast, the live duration cannot be set: using fixed-length variants", label,
                    kTimedFixGiveUpSeconds);
            }
        }

        // Dispels every timed withdrawal spell and removes the old lasting withdrawal abilities.
        bool RemoveAll(RE::PlayerCharacter* player)
        {
            bool ok = true;
            for (int tier = 1; tier <= 3; ++tier) {
                DispelSlot(WithdrawalId, tier);
                if (!Timed::Lookup(Forms::kTimedWithdrawal[tier - 1][0])) ok = false;
                auto* old = Lookup(kAbilityIds[static_cast<std::size_t>(tier - 1)]);
                if (!old) {
                    spdlog::error("Withdrawal: spell for tier {} not found in {}", tier, Forms::kPluginName);
                    ok = false;
                    continue;
                }
                if (player->HasSpell(old)) {
                    const bool removed = player->RemoveSpell(old);
                    spdlog::info("Withdrawal: removed old tier {} ability, RemoveSpell -> {}", tier, removed);
                }
            }
            g_wdTimed = TimedSlot{};
            return ok;
        }

        // Makes the player carry exactly the timed spell for `tier` (none for 0), with `wantedSeconds` left, and keeps its
        // timer in step on later calls. Sets g_applied only on success. `notice` is the HUD text for a change of tier
        // ("" = none); it is shown only when the change succeeded.
        void Apply(RE::PlayerCharacter* player, int tier, const char* why, const std::string& notice, double wantedSeconds, double now,
            bool doseSeen)
        {
            const auto plan = Core::PlanWithdrawal(g_applied, tier);
            if (!plan.change) {
                if (g_applied > 0) EnsureTimed(g_wdTimed, WithdrawalId, g_applied, wantedSeconds, now, doseSeen, fmt::format("Withdrawal: tier {}", g_applied));
                return;
            }
            const int before = g_applied;
            if (!RemoveAll(player)) return;  // retried next tick
            if (plan.addTier > 0) {
                const std::string label = fmt::format("Withdrawal: tier {}", plan.addTier);
                spdlog::info("Withdrawal: tier {} -> {} ({})", before, plan.addTier, why);
                if (!EnsureTimed(g_wdTimed, WithdrawalId, plan.addTier, wantedSeconds, now, false, label)) return;  // retried next tick
                Diag::Schedule(WithdrawalId(plan.addTier, g_wdTimed.idx), fmt::format("withdrawal tier {}", plan.addTier));
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

        // Makes the player carry the timed "Addicted: <drug>" spell exactly for the drugs in `want` (one each, with about
        // wantedSeconds[i] left), and none for the others. A drug whose flag was known to be on and goes off because its
        // dependency fell says so on the HUD (only with addiction enabled).
        void SyncAddicted(RE::PlayerCharacter* player, const std::array<bool, 3>& want, bool announce, const std::array<double, 3>& wantedSeconds,
            double now, const std::array<bool, 3>& doseSeen)
        {
            for (std::size_t i = 0; i < 3; ++i) {
                const auto type = static_cast<Core::DrugType>(i);
                const int  slot = static_cast<int>(i);
                if (g_addicted[i] == -1) {  // unknown (after a load): clear whatever the save carried, the old ability too
                    DispelSlot(AddictedId, slot);
                    g_addTimed[i] = TimedSlot{};
                    if (auto* old = Lookup(kAddictedIds[i]); old && player->HasSpell(old)) {
                        const bool removed = player->RemoveSpell(old);
                        spdlog::info("Addicted: old {} ability removed, RemoveSpell -> {}", Core::DrugDisplayName(type), removed);
                    }
                }
                if (want[i]) {
                    const std::string label = fmt::format("Addicted: {}", Core::DrugDisplayName(type));
                    if (EnsureTimed(g_addTimed[i], AddictedId, slot, wantedSeconds[i], now, doseSeen[i], label)) {
                        if (g_addicted[i] != 1) {
                            spdlog::info("Addicted: {} timed spell on, {:.0f} s until the dependency is below {:.0f}", Core::DrugDisplayName(type),
                                wantedSeconds[i], static_cast<double>(Core::kTier1Level));
                        }
                        g_addicted[i] = 1;
                    }
                    continue;
                }
                const int before = g_addicted[i];
                if (before != 0) {
                    DispelSlot(AddictedId, slot);
                    g_addTimed[i] = TimedSlot{};
                    spdlog::info("Addicted: {} timed spell removed", Core::DrugDisplayName(type));
                }
                g_addicted[i] = 0;
                if (before == 1 && announce) {
                    const auto text = Core::NoLongerAddictedMessage(type);
                    RE::SendHUDMessage::ShowHUDMessage(text.c_str(), nullptr, false);
                    spdlog::info("Addicted: HUD message: {}", text);
                }
            }
        }

        // Fast pass (every kTimedFixPollSeconds): the duration fix of spells that were just cast.
        double g_fixNext = 0.0;
        void   TimedFixTick(double now)
        {
            if (now < g_fixNext) return;
            g_fixNext = now + kTimedFixPollSeconds;
            for (int i = 0; i < 3; ++i) {
                FixPending(g_addTimed[static_cast<std::size_t>(i)], AddictedId, i,
                    now, fmt::format("Addicted: {}", Core::DrugDisplayName(static_cast<Core::DrugType>(i))));
            }
            if (g_applied > 0) FixPending(g_wdTimed, WithdrawalId, g_applied, now, fmt::format("Withdrawal: tier {}", g_applied));
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

    // Casts HighrimRavage with the given amounts (points taken off the maximum of Health, Magicka, Stamina). The magnitudes are
    // written into the spell's effects first (the spell is rebuilt from the plugin at every game start), any live copy goes.
    static void ApplyRavage(const std::array<double, 3>& amounts)
    {
        auto* spell = Timed::Lookup(Forms::HighrimRavage);
        if (!spell) {
            spdlog::error("Rimerush drain: ravage spell {:06X} not found in {}", Forms::HighrimRavage, Forms::kPluginName);
            return;
        }
        const float magnitudes[3] = { -static_cast<float>(amounts[0]), -static_cast<float>(amounts[1]), -static_cast<float>(amounts[2]) };
        Timed::Dispel(spell);
        if (!Timed::SetEffectMagnitudes(spell, magnitudes, 3)) {
            spdlog::error("Rimerush drain: ravage spell has {} effects, expected 3", spell->effects.size());
            return;
        }
        if (Timed::Cast(spell)) {
            g_ravageApplied = amounts;
            spdlog::info("Rimerush drain: ravage cast, maximums lowered by H {:.1f} M {:.1f} S {:.1f}", amounts[0], amounts[1], amounts[2]);
        }
    }

    // Takes the ravage off (the maximums come back). No-op when nothing is applied; an unknown state is cleared too.
    static void ReleaseRavage(const char* why)
    {
        const bool known = g_ravageApplied[0] == 0.0 && g_ravageApplied[1] == 0.0 && g_ravageApplied[2] == 0.0;
        g_ravageWanted   = { 0.0, 0.0, 0.0 };
        if (known) return;
        if (auto* spell = Timed::Lookup(Forms::HighrimRavage)) {
            const int n = Timed::Dispel(spell);
            spdlog::info("Rimerush drain: ravage released ({}), {} effect(s) dispelled", why, n);
        }
        g_ravageApplied = { 0.0, 0.0, 0.0 };
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

        // The blackout ended with the ravage released: restore the bars once the maximums are back (a moment later).
        if (g_restoreAt && now >= *g_restoreAt) {
            g_restoreAt.reset();
            if (!player->IsDead()) RestoreBars(player);
        }

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
                    if (Core::kDrainUsesRavage) {
                        ReleaseRavage("blackout ended");
                        g_restoreAt = now + 1.5;  // RestoreBars runs once the maximums are back
                    } else {
                        RestoreBars(player);
                    }
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
            if (Core::kDrainUsesRavage) ReleaseRavage(g_drainActive ? "drain stopped" : "not active");
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
            if (Core::kDrainUsesRavage && g_ravageApplied[0] < 0.0) {  // a ravage from a save may still be live: clear it first
                ReleaseRavage("unknown state at the start of the drain");
                g_drainLast = now;
                return;
            }
            g_drainActive = true;
            g_drainStage  = 0;
            g_drainLast   = now;
            g_depleted    = false;
            g_lastSet     = { std::nan(""), std::nan(""), std::nan("") };
            spdlog::info("Rimerush drain: started (level {:.1f}, {:.1f} h into withdrawal, {})", level, intoWithdrawal,
                Core::kDrainUsesRavage ? "ravage: lowers the maximums" : "direct drain of the current values");
            RE::SendHUDMessage::ShowHUDMessage(Core::RimerushDrainStartMessage(), nullptr, false);
        }

        const double dt   = std::clamp(now - g_drainLast, 0.0, kDrainMaxStepSeconds);
        g_drainLast       = now;
        const double rate = Core::RimerushDrainPerSecond(level, intoWithdrawal);
        std::array<double, 3> after{};
        double                lowest = 1.0;
        if (Core::kDrainUsesRavage) {
            // Ravage: the drain lowers the MAXIMUM (the part of the bar that is lost); the current value follows it down.
            // `after` is the maximum that is left: the depletion rules below use it (a bar is empty when its maximum is gone).
            std::array<double, 3> baseMax{};
            bool                  recast = false;
            for (std::size_t i = 0; i < 3; ++i) {
                const double applied = g_ravageApplied[i] > 0.0 ? g_ravageApplied[i] : 0.0;
                baseMax[i]           = player->GetActorValueMax(kBars[i]) + applied;  // the maximum without our own ravage
                const double floor   = i == 0 ? Core::kHealthDrainFloor : 0.0;
                g_ravageWanted[i]    = Core::RavageAmountAfter(g_ravageWanted[i], rate, dt, baseMax[i], floor);
                double left          = std::max(0.0, baseMax[i] - g_ravageWanted[i]);
                if (left - floor < 1e-6) left = floor;  // at the cap: exactly the floor (no rounding noise for the depletion check)
                after[i] = left;
                if (baseMax[i] > 0.0) lowest = std::min(lowest, left / baseMax[i]);
                if (Core::RavageRecastNeeded(g_ravageApplied[i], g_ravageWanted[i], Core::kRavageRecastStep)) recast = true;
            }
            if (recast) ApplyRavage(g_ravageWanted);
        } else {
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
        g_ravageApplied = { -1.0, -1.0, -1.0 };  // a ravage the save carried is dispelled at the next idle drain tick
        g_restoreAt.reset();
        g_applied  = -1;
        g_addicted = { -1, -1, -1 };
        g_addTimed = {};
        g_wdTimed  = TimedSlot{};
        g_fixNext  = 0.0;
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
            TimedFixTick(now);
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
            std::array<double, 3> wantedSeconds{};  // per drug: real seconds until its dependency is below 10 (Core::SecondsUntilClear)
            std::array<bool, 3>   doseSeen{};       // per drug: a dose since the previous poll
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
                auto       decayed = snap;
                Core::Decay(decayed, hours);
                const double timescale = Timescale();
                for (std::size_t i = 0; i < 3; ++i) {
                    wantedSeconds[i] = Core::SecondsUntilClear(decayed.level[i], Core::DecayPerDayFor(static_cast<Core::DrugType>(i)), timescale);
                    doseSeen[i]      = g_prev && g_prev->lastDoseHours[i] != snap.lastDoseHours[i];
                }
                g_prev            = snap;
                // The withdrawal timer is the time until the worst drug's dependency is below 10.
                const double withdrawalSeconds = worst ? wantedSeconds[static_cast<std::size_t>(*worst)]
                                                       : *std::max_element(wantedSeconds.begin(), wantedSeconds.end());
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
                Apply(player, tier, "poll", notice, withdrawalSeconds, now, dose);
            }
            SyncAddicted(player, addicted, addictionOn, wantedSeconds, now, doseSeen);

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
