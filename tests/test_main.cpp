#include "Config.h"
#include "Core.h"
#include "Forms.h"
#include "GameData.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            return 1;                                                       \
        }                                                                   \
    } while (0)

namespace
{
    void Write(const std::filesystem::path& a_path, const std::string& a_text)
    {
        std::filesystem::create_directories(a_path.parent_path());
        std::ofstream(a_path) << a_text;
    }
}

namespace
{
    using namespace Highrim::Core;

    int TestCore()
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();

        // Dose stacks: per drug, window 30 min = 0.5 h.
        {
            DoseStacks st{};
            CHECK(AddStackedDose(st, DrugType::kStimulant, 10.0, 30) == 1);   // first dose
            CHECK(AddStackedDose(st, DrugType::kStimulant, 10.0, 30) == 2);   // same instant counts
            CHECK(AddStackedDose(st, DrugType::kStimulant, 10.5, 30) == 3);   // exactly the window
            CHECK(AddStackedDose(st, DrugType::kDepressant, 10.6, 30) == 1);  // other drug has its own stack
            CHECK(AddStackedDose(st, DrugType::kStimulant, 10.9, 30) == 4);   // other drugs do not touch it
            CHECK(AddStackedDose(st, DrugType::kStimulant, 10.9 + 0.5 + 1.0 / 3600, 30) == 1);  // one second over restarts
            CHECK(AddStackedDose(st, DrugType::kDepressant, 5.0, 30) == 1);   // clock went backwards restarts
            CHECK(AddStackedDose(st, DrugType::kDepressant, 5.1, 30) == 2);
            CHECK(AddStackedDose(st, DrugType::kHallucinogen, nan, 30) == 1);  // non-finite restarts at 1
            CHECK(AddStackedDose(st, DrugType::kHallucinogen, 20.0, 30) == 1);  // and leaves no usable last dose
            CHECK(AddStackedDose(st, DrugType::kHallucinogen, 20.1, 30) == 2);
            CHECK(AddStackedDose(st, DrugType::kHallucinogen, inf, 30) == 1);
            CHECK(AddStackedDose(st, DrugType::kHallucinogen, 20.0, 30) == 1);
            CHECK(AddStackedDose(st, DrugType::kHallucinogen, 20.0, 0) == 1);   // bad window restarts
            CHECK(AddStackedDose(st, DrugType::kHallucinogen, 20.0, -5) == 1);
            CHECK(AddStackedDose(st, DrugType::kHallucinogen, 20.0, 1) == 2);
            CHECK(AddStackedDose(st, static_cast<DrugType>(7), 20.0, 30) == 0);  // unknown drug is ignored
            DoseStacks big{};
            int last = 0;
            for (int i = 0; i < 5000; ++i) last = AddStackedDose(big, DrugType::kStimulant, 1.0, 30);
            CHECK(last > 8 && last < 5000);  // capped, never overflows
            ResetStacks(st);
            CHECK(AddStackedDose(st, DrugType::kStimulant, 10.1, 30) == 1);
        }
        CHECK(!ShouldBlackout(3, 4));
        CHECK(ShouldBlackout(4, 4));
        CHECK(ShouldBlackout(5, 4));
        CHECK(!ShouldBlackout(0, 4));
        CHECK(ShouldBlackout(2, 2));
        CHECK(!ShouldBlackout(1, 2));
        CHECK(ShouldBlackout(2, 0));   // a bad setting counts as the minimum of 2
        CHECK(!ShouldBlackout(1, -3));
        CHECK(StackLevel(0) == 0 && StackLevel(1) == 0 && StackLevel(-4) == 0);
        CHECK(StackLevel(2) == 1 && StackLevel(3) == 2 && StackLevel(4) == 3);
        CHECK(StackLevel(5) == 3 && StackLevel(1000) == 3);
        // Combined counter: any drug, same window rules.
        {
            DoseTotals t{};
            CHECK(AddAnyDose(t, 10.0, 30) == 1);
            CHECK(AddAnyDose(t, 10.2, 30) == 2);   // the counter has no drug: any drug counts
            CHECK(AddAnyDose(t, 10.7, 30) == 3);   // 0.5 h after the previous dose: exactly the window
            CHECK(AddAnyDose(t, 11.3, 30) == 1);   // older than the window restarts
            CHECK(AddAnyDose(t, 11.3, 0) == 1);    // bad window restarts
            CHECK(AddAnyDose(t, 11.4, 120) == 2);
            CHECK(AddAnyDose(t, nan, 120) == 1);   // non-finite restarts and leaves no previous dose
            CHECK(AddAnyDose(t, 12.0, 120) == 1);
            CHECK(AddAnyDose(t, 5.0, 120) == 1);   // clock went backwards
            int last = 0;
            for (int i = 0; i < 5000; ++i) last = AddAnyDose(t, 5.0, 30);
            CHECK(last > 8 && last < 5000);
            // Mixed doses reach the blackout count although no single drug stack does.
            DoseStacks st{};
            DoseTotals tot{};
            int total = 0, s0 = 0, s1 = 0;
            s0 = AddStackedDose(st, DrugType::kStimulant, 2.0, 30);     total = AddAnyDose(tot, 2.0, 30);
            s1 = AddStackedDose(st, DrugType::kDepressant, 2.1, 30);    total = AddAnyDose(tot, 2.1, 30);
            s0 = AddStackedDose(st, DrugType::kStimulant, 2.2, 30);     total = AddAnyDose(tot, 2.2, 30);
            s1 = AddStackedDose(st, DrugType::kHallucinogen, 2.3, 30);  total = AddAnyDose(tot, 2.3, 30);
            CHECK(s0 == 2 && s1 == 1 && total == 4 && ShouldBlackout(total, 4) && !ShouldBlackout(s0, 4));
        }
        // Velvet Haze screen strength rises with every stack, capped at 1.
        CHECK(StrengthForVelvetStack(1) == 0.5);
        CHECK(StrengthForVelvetStack(0) == 0.5 && StrengthForVelvetStack(-2) == 0.5);
        CHECK(std::fabs(StrengthForVelvetStack(2) - 0.7) < 1e-9 && std::fabs(StrengthForVelvetStack(3) - 0.9) < 1e-9);
        CHECK(StrengthForVelvetStack(4) == 1.0 && StrengthForVelvetStack(99) == 1.0);
        CHECK(StrengthForVelvetStack(1) < StrengthForVelvetStack(2) && StrengthForVelvetStack(2) < StrengthForVelvetStack(3) &&
              StrengthForVelvetStack(3) < StrengthForVelvetStack(4));
        // Blackout start after the knock.
        CHECK(kRagdollFirstSeconds == 2.5 && BlackoutStartAfterKnock(100.0) == 102.5);
        CHECK(StrengthForStack(1) == 0.6);
        CHECK(StrengthForStack(0) == 0.6 && StrengthForStack(-3) == 0.6);
        CHECK(StrengthForStack(4) == 1.0 && StrengthForStack(8) == 1.0 && StrengthForStack(100000) == 1.0);
        CHECK(StrengthForStack(1) < StrengthForStack(2) && StrengthForStack(2) < StrengthForStack(3) &&
              StrengthForStack(3) < StrengthForStack(4));

        auto fresh = [] {
            Addiction a{};
            for (int i = 0; i < 3; ++i) { a.level[i] = 0.f; a.lastDoseHours[i] = -1.0; }
            a.lastUpdateHours = 0.0;
            return a;
        };

        // Decay.
        {
            Addiction a = fresh();
            a.level[0] = 50.f;
            a.level[1] = 50.f;
            Decay(a, 24.0);  // one day
            CHECK(std::fabs(a.level[0] - 47.f) < 1e-3f);  // Rimerush decays 3 per day
            CHECK(std::fabs(a.level[1] - 45.f) < 1e-3f);  // the others 5
            CHECK(a.lastUpdateHours == 24.0);
            Decay(a, 24.0 * 100);  // floor at 0
            CHECK(a.level[0] == 0.f);
            CHECK(a.level[1] == 0.f);
        }
        {
            Addiction a = fresh();
            a.level[1] = 30.f;
            a.lastUpdateHours = 10.0;
            Decay(a, 5.0);  // negative elapsed: nothing changes
            CHECK(a.level[1] == 30.f && a.lastUpdateHours == 10.0);
            Decay(a, nan);
            CHECK(a.level[1] == 30.f && a.lastUpdateHours == 10.0);
            Decay(a, inf);
            CHECK(a.level[1] == 30.f && a.lastUpdateHours == 10.0);
        }

        // AddDose.
        {
            Addiction a = fresh();
            AddDose(a, DrugType::kDepressant, 5.0);
            CHECK(a.level[1] == 30.f);
            CHECK(a.lastDoseHours[1] == 5.0);
            CHECK(a.lastDoseHours[0] < 0.0 && a.lastDoseHours[2] < 0.0);
            CHECK(a.lastUpdateHours == 5.0);
            AddDose(a, DrugType::kDepressant, 5.0 + 24.0);  // decays 5 first, then +30
            CHECK(std::fabs(a.level[1] - 55.f) < 1e-3f);
            for (int i = 0; i < 10; ++i) AddDose(a, DrugType::kDepressant, 29.0);
            CHECK(a.level[1] == 100.f);
            AddDose(a, DrugType::kHallucinogen, 29.0);
            CHECK(a.level[2] == 30.f);
        }

        // Task 15: Rimerush is the most addictive (per-type dose gain and decay).
        CHECK(kRimerushDoseAmount == 40.f && kDoseAmount == 30.f);
        CHECK(kRimerushDecayPerDay == 3.0 && kDecayPerDay == 5.0);
        CHECK(DoseAmountFor(DrugType::kStimulant) == 40.f);
        CHECK(DoseAmountFor(DrugType::kDepressant) == 30.f && DoseAmountFor(DrugType::kHallucinogen) == 30.f);
        CHECK(DecayPerDayFor(DrugType::kStimulant) == 3.0);
        CHECK(DecayPerDayFor(DrugType::kDepressant) == 5.0 && DecayPerDayFor(DrugType::kHallucinogen) == 5.0);
        {
            Addiction a = fresh();
            AddDose(a, DrugType::kStimulant, 0.0);
            CHECK(a.level[0] == 40.f && a.level[1] == 0.f);
            a.level[1] = 50.f;
            a.level[2] = 50.f;
            AddDose(a, DrugType::kStimulant, 24.0);  // one day: Rimerush -3 then +40, the others -5
            CHECK(std::fabs(a.level[0] - 77.f) < 1e-3f);
            CHECK(std::fabs(a.level[1] - 45.f) < 1e-3f && std::fabs(a.level[2] - 45.f) < 1e-3f);
            for (int i = 0; i < 5; ++i) AddDose(a, DrugType::kStimulant, 24.0);
            CHECK(a.level[0] == 100.f);
        }

        // The full blackout is its own gate.
        CHECK(FullBlackoutAllowed(true) && !FullBlackoutAllowed(false));

        // Overdose rolls (Task 16): chance 15% per dose from the threshold, capped at 60%.
        {
            CHECK(kFullBlackoutChanceStep == 0.15 && kFullBlackoutChanceCap == 0.60 && kFaintChance == 0.5);
            CHECK(FullBlackoutChance(3, 4) == 0.0 && FullBlackoutChance(0, 4) == 0.0);
            CHECK(std::fabs(FullBlackoutChance(4, 4) - 0.15) < 1e-9);
            CHECK(std::fabs(FullBlackoutChance(5, 4) - 0.30) < 1e-9);
            CHECK(std::fabs(FullBlackoutChance(6, 4) - 0.45) < 1e-9);
            CHECK(std::fabs(FullBlackoutChance(7, 4) - 0.60) < 1e-9);
            CHECK(FullBlackoutChance(8, 4) == 0.60 && FullBlackoutChance(99, 4) == 0.60);
            CHECK(std::fabs(FullBlackoutChance(2, 2) - 0.15) < 1e-9);
            CHECK(std::fabs(FullBlackoutChance(2, 0) - 0.15) < 1e-9);   // a bad threshold counts as the minimum of 2
            CHECK(FullBlackoutChance(1, -3) == 0.0);
            // below the threshold nothing is rolled, whatever the random numbers are
            CHECK(RollOverdose(3, 4, 0.0, 0.0) == OverdoseOutcome::kNone);
            // at threshold 4, total 4: full when the first roll is below 15%, else faint when the second is below 50%
            CHECK(RollOverdose(4, 4, 0.0, 0.99) == OverdoseOutcome::kFull);
            CHECK(RollOverdose(4, 4, 0.149, 0.99) == OverdoseOutcome::kFull);
            CHECK(RollOverdose(4, 4, 0.15, 0.0) == OverdoseOutcome::kFaint);   // not below the chance: no full
            CHECK(RollOverdose(4, 4, 0.99, 0.499) == OverdoseOutcome::kFaint);
            CHECK(RollOverdose(4, 4, 0.99, 0.5) == OverdoseOutcome::kNone);
            CHECK(RollOverdose(4, 4, 0.99, 0.99) == OverdoseOutcome::kNone);
            CHECK(RollOverdose(7, 4, 0.59, 0.99) == OverdoseOutcome::kFull && RollOverdose(7, 4, 0.60, 0.99) == OverdoseOutcome::kNone);
            CHECK(RollOverdose(50, 4, 0.61, 0.0) == OverdoseOutcome::kFaint);   // capped at 60%
            CHECK(RollOverdose(4, 4, nan, nan) == OverdoseOutcome::kNone);      // a broken random number never overdoses
            // statistics over an even grid of the two random numbers: full 15%, faint 85% * 50% = 42.5%
            {
                int full = 0, faint = 0, none = 0;
                for (int a = 0; a < 100; ++a)
                    for (int b = 0; b < 100; ++b) {
                        const auto o = RollOverdose(4, 4, (a + 0.5) / 100.0, (b + 0.5) / 100.0);
                        (o == OverdoseOutcome::kFull ? full : o == OverdoseOutcome::kFaint ? faint : none)++;
                    }
                CHECK(full == 1500 && faint == 4250 && none == 4250);
            }
            // risk tier: 0 none, 1 low (1 to threshold-2), 2 elevated (threshold-1), 3 high (threshold or more)
            CHECK(OverdoseRiskTier(0, 4) == RiskTier::kNone && OverdoseRiskTier(-1, 4) == RiskTier::kNone);
            CHECK(OverdoseRiskTier(1, 4) == RiskTier::kLow && OverdoseRiskTier(2, 4) == RiskTier::kLow);
            CHECK(OverdoseRiskTier(3, 4) == RiskTier::kElevated);
            CHECK(OverdoseRiskTier(4, 4) == RiskTier::kHigh && OverdoseRiskTier(9, 4) == RiskTier::kHigh);
            CHECK(OverdoseRiskTier(1, 8) == RiskTier::kLow && OverdoseRiskTier(6, 8) == RiskTier::kLow);
            CHECK(OverdoseRiskTier(7, 8) == RiskTier::kElevated && OverdoseRiskTier(8, 8) == RiskTier::kHigh);
            // threshold 3: low is total 1; threshold 2: low is skipped (1 is already elevated)
            CHECK(OverdoseRiskTier(1, 3) == RiskTier::kLow && OverdoseRiskTier(2, 3) == RiskTier::kElevated);
            CHECK(OverdoseRiskTier(1, 2) == RiskTier::kElevated && OverdoseRiskTier(2, 2) == RiskTier::kHigh);
            CHECK(OverdoseRiskTier(1, 0) == RiskTier::kElevated && OverdoseRiskTier(2, 1) == RiskTier::kHigh);  // threshold below 2 counts as 2
            CHECK(static_cast<int>(RiskTier::kNone) == 0 && static_cast<int>(RiskTier::kHigh) == 3);
            CHECK(std::string(RiskTierName(RiskTier::kLow)) == "Overdose Risk: Low");
            CHECK(std::string(RiskTierName(RiskTier::kElevated)) == "Overdose Risk: Elevated");
            CHECK(std::string(RiskTierName(RiskTier::kHigh)) == "Overdose Risk: High");
            CHECK(std::string(RiskTierName(RiskTier::kNone)).empty());
            CHECK(RiskVisualStrength(RiskTier::kNone) == 0.0 && RiskVisualStrength(RiskTier::kLow) == 0.0);
            CHECK(RiskVisualStrength(RiskTier::kElevated) == 0.4 && RiskVisualStrength(RiskTier::kHigh) == 0.8);
            CHECK(RiskTierRose(RiskTier::kNone, RiskTier::kLow) && RiskTierRose(RiskTier::kLow, RiskTier::kHigh));
            CHECK(!RiskTierRose(RiskTier::kHigh, RiskTier::kHigh) && !RiskTierRose(RiskTier::kHigh, RiskTier::kLow));
            CHECK(!RiskTierRose(RiskTier::kLow, RiskTier::kNone));
            // the dose total counts only while it is inside the window of the last dose
            {
                DoseTotals t{};
                CHECK(ActiveDoseTotal(t, 10.0, 120) == 0);          // no dose yet
                AddAnyDose(t, 10.0, 120);
                AddAnyDose(t, 10.5, 120);
                CHECK(ActiveDoseTotal(t, 10.5, 120) == 2);
                CHECK(ActiveDoseTotal(t, 12.5, 120) == 2);          // exactly the window
                CHECK(ActiveDoseTotal(t, 12.6, 120) == 0);          // expired
                CHECK(ActiveDoseTotal(t, 9.0, 120) == 0);           // clock went backwards: not inside the window
                CHECK(ActiveDoseTotal(t, 10.6, 0) == 0);            // bad window
                CHECK(ActiveDoseTotal(t, nan, 120) == 0);
            }
        }
        // Faint: a hangover of 4 game hours that never shortens a longer one.
        {
            CHECK(kFaintHangoverHours == 4.0 && kFaintGameHours == 1.0 && kFaintTotalSeconds == 6.0);
            CHECK(FaintHangoverExpiry(0.0, 100.0) == 104.0);
            CHECK(FaintHangoverExpiry(101.0, 100.0) == 104.0);        // a shorter one is replaced
            CHECK(FaintHangoverExpiry(124.0, 100.0) == 124.0);        // a longer one stays
            CHECK(FaintHangoverExpiry(nan, 100.0) == 104.0 && FaintHangoverExpiry(-5.0, 100.0) == 104.0);
            CHECK(FaintHangoverExpiry(50.0, nan) == 50.0);            // no usable clock: nothing changes
            CHECK(std::string(FaintStartMessage()) == "You pass out for a moment...");
            CHECK(std::string(FaintEndMessage()) == "You come to, head pounding.");
            CHECK(std::string(RiskRiseMessage(RiskTier::kLow)) == "Overdose Risk: Low. Your body is handling it, for now.");
            CHECK(std::string(RiskRiseMessage(RiskTier::kNone)).empty());
        }

        // Rimerush withdrawal drain.
        CHECK(RimerushDrainActive(10.f, 4.0) && !RimerushDrainActive(9.9f, 50.0) && !RimerushDrainActive(50.f, 3.99));
        {
            CHECK(std::fabs(RimerushDrainPerSecond(10.0, 0.0) - kRimerushDrainBase * 0.7) < 1e-9);
            CHECK(std::fabs(RimerushDrainPerSecond(100.0, 0.0) - kRimerushDrainBase * 2.5) < 1e-9);
            CHECK(RimerushDrainPerSecond(10.0, 0.0) > 0.6 && RimerushDrainPerSecond(10.0, 0.0) < 0.75);
            CHECK(RimerushDrainPerSecond(100.0, 0.0) > 2.3 && RimerushDrainPerSecond(100.0, 0.0) < 2.55);
            CHECK(RimerushDrainPerSecond(60.0, 0.0) > RimerushDrainPerSecond(30.0, 0.0));
            CHECK(std::fabs(RimerushDrainPerSecond(50.0, 6.0) - RimerushDrainPerSecond(50.0, 0.0) * 1.5) < 1e-9);
            CHECK(std::fabs(RimerushDrainPerSecond(50.0, 12.0) - RimerushDrainPerSecond(50.0, 0.0) * 2.0) < 1e-9);
            CHECK(std::fabs(RimerushDrainPerSecond(50.0, 24.0) - RimerushDrainPerSecond(50.0, 0.0) * 3.0) < 1e-9);
            CHECK(RimerushDrainPerSecond(50.0, 1000.0) == RimerushDrainPerSecond(50.0, 24.0));
            CHECK(RimerushDrainPerSecond(9.0, 5.0) == 0.0);
            CHECK(RimerushDrainPerSecond(nan, 5.0) == 0.0 && RimerushDrainPerSecond(50.0, nan) == 0.0);
            CHECK(RimerushDrainPerSecond(500.0, 0.0) == RimerushDrainPerSecond(100.0, 0.0));
            CHECK(RimerushDrainPerSecond(50.0, -3.0) == RimerushDrainPerSecond(50.0, 0.0));
        }
        {
            CHECK(DrainedValue(100.0, 2.0, 1.0, 0.0) == 98.0);
            CHECK(DrainedValue(1.0, 2.0, 1.0, 0.0) == 0.0);
            CHECK(DrainedValue(8.0, 2.0, 5.0, kHealthDrainFloor) == kHealthDrainFloor);
            CHECK(DrainedValue(3.0, 2.0, 1.0, kHealthDrainFloor) == 3.0);
            CHECK(DrainedValue(50.0, nan, 1.0, 0.0) == 50.0 && DrainedValue(50.0, 1.0, nan, 0.0) == 50.0);
            CHECK(DrainedValue(50.0, -1.0, 1.0, 0.0) == 50.0);
            CHECK(kHealthDrainFloor == 5.0);
            // regeneration is suppressed: a small rise since the value the drain left is cancelled, a big one (a potion) is not
            CHECK(DrainBaseValue(52.0, 50.0, kRegenRiseLimit) == 50.0);
            CHECK(DrainBaseValue(50.0 + kRegenRiseLimit, 50.0, kRegenRiseLimit) == 50.0);
            CHECK(DrainBaseValue(50.0 + kRegenRiseLimit + 1.0, 50.0, kRegenRiseLimit) == 50.0 + kRegenRiseLimit + 1.0);
            CHECK(DrainBaseValue(40.0, 50.0, kRegenRiseLimit) == 40.0);  // fell: take the lower value
            CHECK(DrainBaseValue(40.0, nan, kRegenRiseLimit) == 40.0);   // no previous value
            CHECK(!DrainDepleted(6.0, 1.0, 1.0));
            CHECK(DrainDepleted(5.0, 50.0, 50.0) && DrainDepleted(50.0, 0.0, 50.0) && DrainDepleted(50.0, 50.0, 0.0));
            CHECK(DrainDepleted(2.0, 50.0, 50.0));
            CHECK(DrainWarningStage(1.0) == 0 && DrainWarningStage(0.51) == 0);
            CHECK(DrainWarningStage(0.5) == 1 && DrainWarningStage(0.26) == 1);
            CHECK(DrainWarningStage(0.25) == 2 && DrainWarningStage(0.0) == 2);
            CHECK(DrainWarningStage(nan) == 0);
            CHECK(kDrainPauseSeconds == 300.0 && kDrainBlackoutRestoreFraction == 0.25);
            CHECK(std::string(RimerushDrainStartMessage()) == "Rimerush withdrawal: you are fading. You need more Rimerush.");
            CHECK(std::string(RimerushDrainStopMessage()) == "The Rimerush rush returns.");
        }

        // WithdrawalTier.
        CHECK(WithdrawalTier(9.9f, 100.0) == 0);
        CHECK(WithdrawalTier(10.f, 100.0) == 1);
        CHECK(WithdrawalTier(34.9f, 100.0) == 1);
        CHECK(WithdrawalTier(35.f, 100.0) == 2);
        CHECK(WithdrawalTier(59.9f, 100.0) == 2);
        CHECK(WithdrawalTier(60.f, 100.0) == 3);
        CHECK(WithdrawalTier(100.f, 100.0) == 3);
        CHECK(WithdrawalTier(60.f, 3.99) == 0);
        CHECK(WithdrawalTier(60.f, 4.0) == 3);

        // HighestTier decays a copy and does not mutate.
        {
            Addiction a = fresh();
            a.level[0] = 15.f; a.lastDoseHours[0] = 0.0;
            a.level[2] = 75.f; a.lastDoseHours[2] = 0.0;
            CHECK(HighestTier(a, 4.0) == 3);
            CHECK(a.level[2] == 75.f && a.lastUpdateHours == 0.0);
            CHECK(HighestTier(a, 3.5) == 0);
            Addiction b = fresh();
            b.level[1] = 50.f;  // never dosed (negative): treated as long ago, tier applies
            CHECK(HighestTier(b, 1.0) == 2);
        }

        // WorstDrugForTier: highest tier wins, a tie goes to the lowest type index, none in withdrawal -> nullopt.
        {
            Addiction a = fresh();
            CHECK(!WorstDrugForTier(a, 100.0));
            a.level[0] = 15.f; a.lastDoseHours[0] = 0.0;   // tier 1
            a.level[1] = 40.f; a.lastDoseHours[1] = 0.0;   // tier 2
            a.level[2] = 15.f; a.lastDoseHours[2] = 0.0;   // tier 1
            CHECK(WorstDrugForTier(a, 4.0) == DrugType::kDepressant);
            a.level[2] = 70.f;                              // tier 3
            CHECK(WorstDrugForTier(a, 4.0) == DrugType::kHallucinogen);
            a.level[0] = 70.f;                              // tie at tier 3: lowest index
            CHECK(WorstDrugForTier(a, 4.0) == DrugType::kStimulant);
            a.level[1] = 70.f;
            CHECK(WorstDrugForTier(a, 4.0) == DrugType::kStimulant);
            CHECK(!WorstDrugForTier(a, 3.9));               // inside the grace of every drug
            // Per-drug relief: a fresh dose of Rimerush ends only Rimerush's withdrawal.
            a.lastDoseHours[0] = 3.0;
            CHECK(WorstDrugForTier(a, 4.0) == DrugType::kDepressant);
            CHECK(HighestTier(a, 4.0) == 3);
            CHECK(!WorstDrugForTier(a, 3.9));
            // Decay is applied to a copy (5 per day): 66 -> 61 (tier 3) after 24 h, 56 (tier 2) after 48 h.
            Addiction b = fresh();
            b.level[1] = 66.f; b.lastDoseHours[1] = 0.0;
            CHECK(WorstDrugForTier(b, 24.0) == DrugType::kDepressant);
            CHECK(HighestTier(b, 24.0) == 3);
            CHECK(WorstDrugForTier(b, 48.0) == DrugType::kDepressant);
            CHECK(HighestTier(b, 48.0) == 2);
            CHECK(b.level[1] == 66.f);
        }

        // IsAddicted / AddictedFlags: dependency level >= 10 after decay.
        {
            CHECK(!IsAddicted(9.99f) && IsAddicted(10.f) && IsAddicted(100.f) && !IsAddicted(0.f));
            Addiction a = fresh();
            a.level[0] = 10.f; a.level[1] = 9.9f; a.level[2] = 30.f;
            auto f = AddictedFlags(a, 0.0);
            CHECK(f[0] && !f[1] && f[2]);
            f = AddictedFlags(a, 24.0);  // each loses 5 per day: 5, 4.9, 25
            CHECK(!f[0] && !f[1] && f[2]);
            CHECK(a.level[0] == 10.f);   // the copy was decayed, not a
        }

        // DoseSince: a recorded dose changes some drug's last dose time.
        {
            Addiction before = fresh();
            Addiction now    = before;
            CHECK(!DoseSince(before, now));
            now.lastDoseHours[1] = 12.0;
            CHECK(DoseSince(before, now));
            before = now;
            CHECK(!DoseSince(before, now));
            now.lastDoseHours[1] = 13.0;
            CHECK(DoseSince(before, now));
        }

        // Serialize / Deserialize.
        {
            Addiction a = fresh();
            a.level[0] = 12.3456789f; a.level[1] = 100.f; a.level[2] = 0.f;
            a.lastDoseHours[0] = 123.456789012345; a.lastDoseHours[1] = -1.0; a.lastDoseHours[2] = 0.1;
            a.lastUpdateHours = 987.654321098765;
            const std::string text = Serialize(a);
            CHECK(text.rfind("HR1;", 0) == 0);
            auto b = Deserialize(text);
            CHECK(b.has_value());
            for (int i = 0; i < 3; ++i) {
                CHECK(b->level[i] == a.level[i]);
                CHECK(b->lastDoseHours[i] == a.lastDoseHours[i]);
            }
            CHECK(b->lastUpdateHours == a.lastUpdateHours);
            CHECK(Serialize(*b) == text);

            CHECK(!Deserialize("").has_value());
            CHECK(!Deserialize("HR2;1;2;3;-1;-1;-1;0").has_value());
            CHECK(!Deserialize("HR1").has_value());
            CHECK(!Deserialize("HR1;1;2;3;-1;-1;-1").has_value());          // missing field
            CHECK(!Deserialize("HR1;1;2;3;-1;-1;-1;0;9").has_value());      // extra field
            CHECK(!Deserialize("HR1;1;2;3;-1;-1;-1;0;").has_value());       // trailing separator
            CHECK(!Deserialize("HR1;nan;2;3;-1;-1;-1;0").has_value());
            CHECK(!Deserialize("HR1;1;2;3;-1;inf;-1;0").has_value());
            CHECK(!Deserialize("HR1;1;2;3;-1;-1;-1;nan").has_value());
            CHECK(!Deserialize("HR1;101;2;3;-1;-1;-1;0").has_value());
            CHECK(!Deserialize("HR1;-0.5;2;3;-1;-1;-1;0").has_value());
            CHECK(!Deserialize("HR1;1x;2;3;-1;-1;-1;0").has_value());
            CHECK(!Deserialize("HR1;;2;3;-1;-1;-1;0").has_value());
            CHECK(!Deserialize("HR1; 1;2;3;-1;-1;-1;0").has_value());
            CHECK(Deserialize("HR1;0;50;100;-1;-1;-1;0").has_value());
        }

        // NearestIndex.
        {
            const Point none[1] = {};
            CHECK(!NearestIndex({ 0.f, 0.f }, std::span<const Point>(none, 0)).has_value());
            const Point pts[] = { { 10.f, 0.f }, { 0.f, 3.f }, { 0.f, -3.f }, { 5.f, 5.f } };
            auto n = NearestIndex({ 0.f, 0.f }, pts);
            CHECK(n && *n == 1);  // tie between 1 and 2 -> lowest index
            const Point one[] = { { 7.f, 7.f } };
            CHECK(NearestIndex({ 0.f, 0.f }, one) == std::optional<size_t>(0));
        }

        // PickWakeSpot: distances from origin 0,0.
        {
            const Point none[1] = {};
            CHECK(!PickWakeSpot(std::span<const Point>(none, 0), { 0, 0 }, 1.f, 10.f, 0.5f).has_value());
            const Point c[] = { { 0.5f, 0.f }, { 1.f, 0.f }, { 5.f, 0.f }, { 10.f, 0.f }, { 11.f, 0.f } };
            // in band: indices 1, 2, 3 (edges inclusive)
            CHECK(PickWakeSpot(c, { 0, 0 }, 1.f, 10.f, 0.0f) == std::optional<size_t>(1));
            CHECK(PickWakeSpot(c, { 0, 0 }, 1.f, 10.f, 0.32f) == std::optional<size_t>(1));
            CHECK(PickWakeSpot(c, { 0, 0 }, 1.f, 10.f, 0.5f) == std::optional<size_t>(2));
            CHECK(PickWakeSpot(c, { 0, 0 }, 1.f, 10.f, 0.99f) == std::optional<size_t>(3));
            // rng clamping
            CHECK(PickWakeSpot(c, { 0, 0 }, 1.f, 10.f, -3.f) == std::optional<size_t>(1));
            CHECK(PickWakeSpot(c, { 0, 0 }, 1.f, 10.f, 1.0f) == std::optional<size_t>(3));
            CHECK(PickWakeSpot(c, { 0, 0 }, 1.f, 10.f, 7.f) == std::optional<size_t>(3));
            CHECK(PickWakeSpot(c, { 0, 0 }, 1.f, 10.f, std::numeric_limits<float>::quiet_NaN()) == std::optional<size_t>(1));
            // none in band
            CHECK(!PickWakeSpot(c, { 0, 0 }, 20.f, 30.f, 0.5f).has_value());
            CHECK(!PickWakeSpot(c, { 0, 0 }, 6.f, 9.f, 0.5f).has_value());
        }

        // PickWakeSpotStaged: stage 1 15000-80000, stage 2 8000-150000, stage 3 3000-unbounded; never closer than 3000.
        {
            const std::span<const Point> noPoints;
            CHECK(!PickWakeSpotStaged(noPoints, { 0, 0 }, 0.5f).has_value());
            // Only stage 1 candidates (and a too-close one): stage 1.
            const Point a[] = { { 2000.f, 0.f }, { 20000.f, 0.f }, { 0.f, -50000.f }, { 100000.f, 0.f } };
            auto p1 = PickWakeSpotStaged(a, { 0, 0 }, 0.0f);
            CHECK(p1 && p1->stage == 1 && p1->index == 1 && p1->inBand == 2);
            CHECK(std::fabs(p1->distance - 20000.f) < 1.f);
            auto p1b = PickWakeSpotStaged(a, { 0, 0 }, 0.99f);
            CHECK(p1b && p1b->stage == 1 && p1b->index == 2);
            // Nothing in 15000-80000 but something in 8000-150000: stage 2.
            const Point b[] = { { 2999.f, 0.f }, { 9000.f, 0.f }, { 120000.f, 0.f } };
            auto p2 = PickWakeSpotStaged(b, { 0, 0 }, 0.99f);
            CHECK(p2 && p2->stage == 2 && p2->index == 2 && p2->inBand == 2);
            // Only 3000..8000 or beyond 150000: stage 3 (unbounded above), edge 3000 inclusive.
            const Point c[] = { { 1000.f, 0.f }, { 3000.f, 0.f }, { 200000.f, 0.f } };
            auto p3 = PickWakeSpotStaged(c, { 0, 0 }, 0.0f);
            CHECK(p3 && p3->stage == 3 && p3->index == 1 && p3->inBand == 2);
            auto p3b = PickWakeSpotStaged(c, { 0, 0 }, 1.0f);
            CHECK(p3b && p3b->stage == 3 && p3b->index == 2);
            // Everything closer than 3000: no candidates in any stage.
            const Point d[] = { { 0.f, 0.f }, { 2999.f, 0.f } };
            CHECK(!PickWakeSpotStaged(d, { 0, 0 }, 0.5f).has_value());
            // Origin is honoured and rng is clamped (NaN -> first).
            const Point e[] = { { 100000.f, 100000.f }, { 120000.f, 100000.f }, { 190000.f, 100000.f } };
            auto p4 = PickWakeSpotStaged(e, { 100000.f, 100000.f }, std::numeric_limits<float>::quiet_NaN());
            CHECK(p4 && p4->stage == 1 && p4->index == 1);
            auto p5 = PickWakeSpotStaged(e, { 100000.f, 100000.f }, 9.f);
            CHECK(p5 && p5->index == 1);  // 90000 away is outside stage 1, only index 1 (20000) is in
        }

        // Hold choice: capitals by worldspace id, otherwise nearest by Tamriel-space anchor; only holds with a chest.
        {
            const HoldInfo holds[] = {
                { true, { 0.f, 0.f } },        // 0 Whiterun-like, city world 0x100
                { true, { 100000.f, 0.f } },   // 1 city world 0x200
                { true, { 50000.f, 90000.f } },  // 2 no city world
                { false, { 40000.f, 0.f } },   // 3 no chest (Winterhold-like), nearest to many points
            };
            const WorldHold worlds[] = { { 0x100, 0 }, { 0x200, 1 }, { 0x300, 0 }, { 0x400, 3 } };
            CHECK(!DirectHoldForWorld(0, worlds, holds).has_value());
            CHECK(DirectHoldForWorld(0x100, worlds, holds) == std::optional<std::size_t>(0));
            CHECK(DirectHoldForWorld(0x300, worlds, holds) == std::optional<std::size_t>(0));  // second world of a hold
            CHECK(!DirectHoldForWorld(0x400, worlds, holds).has_value());  // that hold has no chest
            CHECK(!DirectHoldForWorld(0x3C, worlds, holds).has_value());
            // In a city worldspace: that hold, whatever the position says.
            auto c1 = ChooseHold(holds, worlds, 0x200, Point{ 0.f, 0.f });
            CHECK(c1 && c1->index == 1 && c1->reason == HoldReason::kCapitalWorld);
            auto c2 = ChooseHold(holds, worlds, 0x100, std::nullopt);
            CHECK(c2 && c2->index == 0 && c2->reason == HoldReason::kCapitalWorld);
            // In the wilderness: nearest, skipping the hold without a chest even though it is closest.
            auto c3 = ChooseHold(holds, worlds, 0x3C, Point{ 41000.f, 0.f });
            CHECK(c3 && c3->index == 0 && c3->reason == HoldReason::kNearest);
            auto c4 = ChooseHold(holds, worlds, 0x3C, Point{ 90000.f, 10000.f });
            CHECK(c4 && c4->index == 1);
            auto c5 = ChooseHold(holds, worlds, 0x3C, Point{ 50000.f, 80000.f });
            CHECK(c5 && c5->index == 2);
            // Unknown position and not in a city: nothing to choose.
            CHECK(!ChooseHold(holds, worlds, 0x3C, std::nullopt).has_value());
            const HoldInfo nochest[] = { { false, { 0.f, 0.f } } };
            CHECK(!ChooseHold(nochest, worlds, 0x3C, Point{ 1.f, 1.f }).has_value());
        }

        // The generated Skyrim.esm table feeds the hold choice: capitals by worldspace, nearest otherwise.
        {
            using namespace Highrim::GameData;
            std::vector<HoldInfo>  holds;
            std::vector<WorldHold> worlds;
            for (std::size_t i = 0; i < std::size(kHoldAnchors); ++i) {
                const auto& a = kHoldAnchors[i];
                // Winterhold (the last entry, 0x0002816F) has no confiscation chest in vanilla.
                holds.push_back({ a.factionId != 0x0002816F, { a.x, a.y } });
                if (a.cityWorld != 0) {
                    worlds.push_back({ a.cityWorld, i });
                }
            }
            CHECK(holds.size() == 9);
            for (const auto& w : kWorlds) {
                if (w.holdFaction != 0) {
                    bool found = false;
                    for (std::size_t i = 0; i < std::size(kHoldAnchors); ++i) {
                        found = found || kHoldAnchors[i].factionId == w.holdFaction;
                    }
                    CHECK(found);
                }
            }
            // Windhelm's worldspace (0001691D) is Eastmarch, Whiterun's (0001A26F) is Whiterun, whatever the position.
            auto win = ChooseHold(holds, worlds, 0x0001691D, Point{ -150000.f, 0.f });
            CHECK(win && kHoldAnchors[win->index].factionId == 0x000267E3 && win->reason == HoldReason::kCapitalWorld);
            auto wr = ChooseHold(holds, worlds, 0x0001A26F, std::nullopt);
            CHECK(wr && kHoldAnchors[wr->index].factionId == 0x000267EA);
            // Wilderness: next to Falkreath's jail marker and to Winterhold's (which has no chest: Pale is picked instead).
            auto fk = ChooseHold(holds, worlds, 0x3C, Point{ -29000.f, -87000.f });
            CHECK(fk && kHoldAnchors[fk->index].factionId == 0x00028170 && fk->reason == HoldReason::kNearest);
            auto wh = ChooseHold(holds, worlds, 0x3C, Point{ 109783.f, 100980.f });
            CHECK(wh && kHoldAnchors[wh->index].factionId != 0x0002816F);
            // The marker table: plenty of unique ids, strictly sorted, inside the map.
            CHECK(std::size(kTamrielMarkers) > 200);
            for (std::size_t i = 0; i < std::size(kTamrielMarkers); ++i) {
                CHECK(i == 0 || kTamrielMarkers[i - 1].id < kTamrielMarkers[i].id);
                CHECK(std::fabs(kTamrielMarkers[i].x) < 250000.f && std::fabs(kTamrielMarkers[i].y) < 250000.f);
            }
        }

        // Worldspace relations: 3C Tamriel, 100 Whiterun city (child of 3C), 200 Solitude city, 300 a separate root.
        {
            const WorldParents wp{ { 0x100, 0x3C }, { 0x200, 0x3C }, { 0x3C, 0 }, { 0x400, 0x100 } };
            CHECK(WorldRoot(0x3C, wp) == 0x3C);
            CHECK(WorldRoot(0x100, wp) == 0x3C);
            CHECK(WorldRoot(0x400, wp) == 0x3C);
            CHECK(WorldRoot(0x300, wp) == 0x300);
            CHECK(IsDescendantWorld(0x100, 0x3C, wp));
            CHECK(IsDescendantWorld(0x400, 0x3C, wp));
            CHECK(!IsDescendantWorld(0x3C, 0x100, wp));
            CHECK(!IsDescendantWorld(0x100, 0x100, wp));
            CHECK(!IsDescendantWorld(0x100, 0x200, wp));
            CHECK(!IsDescendantWorld(0, 0x3C, wp));
            CHECK(ClassifyAnchorSpace(0x3C, 0x3C, wp) == AnchorSpace::kSame);
            CHECK(ClassifyAnchorSpace(0, 0x3C, wp) == AnchorSpace::kSame);
            CHECK(ClassifyAnchorSpace(0x100, 0x3C, wp) == AnchorSpace::kChild);
            CHECK(ClassifyAnchorSpace(0x100, 0x100, wp) == AnchorSpace::kSame);
            CHECK(ClassifyAnchorSpace(0x100, 0x200, wp) == AnchorSpace::kUnrelated);
            CHECK(ClassifyAnchorSpace(0x3C, 0x100, wp) == AnchorSpace::kUnrelated);
            const WorldParents cyc{ { 1, 2 }, { 2, 1 } };
            (void)WorldRoot(1, cyc);
            CHECK(!IsDescendantWorld(1, 9, cyc));
        }

        // Overdose deferral.
        CHECK(DecideOverdose(10.0, 11.5, false) == OverdoseAction::kWait);   // not due yet
        CHECK(DecideOverdose(11.5, 11.5, false) == OverdoseAction::kRun);
        CHECK(DecideOverdose(12.0, 11.5, true) == OverdoseAction::kWait);    // menu open
        CHECK(DecideOverdose(41.4, 11.5, true) == OverdoseAction::kWait);    // 29.9 s of waiting
        CHECK(DecideOverdose(41.5, 11.5, true) == OverdoseAction::kDrop);    // 30 s
        CHECK(DecideOverdose(100.0, 11.5, true) == OverdoseAction::kDrop);
        CHECK(DecideOverdose(41.4, 11.5, false) == OverdoseAction::kRun);    // menu closed just in time
        CHECK(DecideOverdose(nan, 11.5, false) == OverdoseAction::kDrop);
        CHECK(DecideOverdose(11.5, inf, false) == OverdoseAction::kDrop);

        // Drug id mapping.
        CHECK(DrugFromLocalId(Highrim::Forms::HighrimRimerush) == std::optional<DrugType>(DrugType::kStimulant));
        CHECK(DrugFromLocalId(Highrim::Forms::HighrimVelvetHaze) == std::optional<DrugType>(DrugType::kDepressant));
        CHECK(DrugFromLocalId(Highrim::Forms::HighrimDreamcap) == std::optional<DrugType>(DrugType::kHallucinogen));
        CHECK(!DrugFromLocalId(Highrim::Forms::HighrimDrug).has_value());
        CHECK(!DrugFromLocalId(0).has_value());

        // Persisted state (addiction + hangover expiry).
        {
            PersistedState p{};
            p.addiction.level[1] = 42.5f; p.addiction.lastDoseHours[1] = 123.456; p.addiction.lastUpdateHours = 124.0;
            p.hangoverExpiryHours = 987.654321;
            const std::string t = SerializeState(p);
            auto q = DeserializeState(t);
            CHECK(q.has_value());
            CHECK(q->hangoverExpiryHours == p.hangoverExpiryHours);
            CHECK(Serialize(q->addiction) == Serialize(p.addiction));
            CHECK(SerializeState(*q) == t);
            CHECK(!DeserializeState("").has_value());
            CHECK(!DeserializeState("HS1").has_value());
            CHECK(!DeserializeState("HS2|" + Serialize(p.addiction) + "|1").has_value());
            CHECK(!DeserializeState("HS1|garbage|1").has_value());
            CHECK(!DeserializeState("HS1|" + Serialize(p.addiction)).has_value());
            CHECK(!DeserializeState("HS1|" + Serialize(p.addiction) + "|").has_value());
            CHECK(!DeserializeState("HS1|" + Serialize(p.addiction) + "|-1").has_value());
            CHECK(!DeserializeState("HS1|" + Serialize(p.addiction) + "|nan").has_value());
            CHECK(!DeserializeState("HS1|" + Serialize(p.addiction) + "|1|2").has_value());
            // Version 1 (no Lost Gear fields) still loads, as "no reminder".
            auto v1 = DeserializeState("HS1|" + Serialize(p.addiction) + "|55.5");
            CHECK(v1.has_value() && v1->hangoverExpiryHours == 55.5 && v1->lostGearHold == -1 && v1->lostGearExpiryHours == 0.0);
            // Version 2 carries the reminder.
            p.lostGearHold = 4; p.lostGearExpiryHours = 1234.5;
            auto v2 = DeserializeState(SerializeState(p));
            CHECK(v2.has_value() && v2->lostGearHold == 4 && v2->lostGearExpiryHours == 1234.5 && v2->hangoverExpiryHours == p.hangoverExpiryHours);
            CHECK(SerializeState(p).starts_with("HS2|"));
            p.lostGearHold = -1; p.lostGearExpiryHours = 0.0;
            CHECK(DeserializeState(SerializeState(p))->lostGearHold == -1);
            CHECK(!DeserializeState("HS2|" + Serialize(p.addiction) + "|1|2").has_value());          // too few fields
            CHECK(!DeserializeState("HS2|" + Serialize(p.addiction) + "|1|2|3|4").has_value());      // too many
            CHECK(!DeserializeState("HS2|" + Serialize(p.addiction) + "|1|-2|3").has_value());       // hold below -1
            CHECK(!DeserializeState("HS2|" + Serialize(p.addiction) + "|1|100|3").has_value());      // hold out of range
            CHECK(!DeserializeState("HS2|" + Serialize(p.addiction) + "|1|2|-3").has_value());       // negative expiry
            CHECK(!DeserializeState("HS2|" + Serialize(p.addiction) + "|1|2|nan").has_value());
        }

        // Comedown visual strength: 0.5 + 0.15 per stack, capped at 1.0.
        CHECK(std::fabs(StrengthForComedown(1) - 0.65) < 1e-9);
        CHECK(std::fabs(StrengthForComedown(2) - 0.80) < 1e-9);
        CHECK(std::fabs(StrengthForComedown(3) - 0.95) < 1e-9);
        CHECK(StrengthForComedown(4) == 1.0);
        CHECK(StrengthForComedown(99) == 1.0);
        CHECK(std::fabs(StrengthForComedown(0) - 0.65) < 1e-9);   // below 1 counts as 1
        CHECK(std::fabs(StrengthForComedown(-5) - 0.65) < 1e-9);

        // Messages.
        CHECK(ComedownMessage(DrugType::kStimulant) == "Rimerush wears off: stamina regenerates 60% slower for 60 s.");
        CHECK(ComedownMessage(DrugType::kDepressant).starts_with("Velvet Haze wears off"));
        CHECK(ComedownMessage(DrugType::kHallucinogen).starts_with("Dreamcap wears off"));
        CHECK(WithdrawalMessage(1, DrugType::kStimulant) == "Withdrawal I: cravings gnaw at you. You need Rimerush.");
        CHECK(WithdrawalMessage(2, DrugType::kStimulant) == "Withdrawal II: your hands shake. You need Rimerush.");
        CHECK(WithdrawalMessage(3, DrugType::kDepressant) == "Withdrawal III: you feel sick. You need Velvet Haze.");
        CHECK(WithdrawalMessage(2, DrugType::kHallucinogen).ends_with("You need Dreamcap."));
        CHECK(WithdrawalMessage(0, DrugType::kStimulant).empty() && WithdrawalMessage(4, DrugType::kStimulant).empty());
        CHECK(CravingsEaseMessage() == "The cravings ease.");
        CHECK(NoLongerAddictedMessage(DrugType::kHallucinogen) == "You are no longer addicted to Dreamcap.");
        CHECK(DrugDisplayName(DrugType::kDepressant) == "Velvet Haze");
        CHECK(ClassifyWithdrawalChange(0, 1, false) == WithdrawalNotice::kRaised);
        CHECK(ClassifyWithdrawalChange(1, 3, true) == WithdrawalNotice::kRaised);
        CHECK(ClassifyWithdrawalChange(2, 0, true) == WithdrawalNotice::kEased);
        CHECK(ClassifyWithdrawalChange(3, 2, false) == WithdrawalNotice::kNone);  // decay alone says nothing
        CHECK(ClassifyWithdrawalChange(2, 2, true) == WithdrawalNotice::kNone);
        CHECK(ClassifyWithdrawalChange(-1, 3, false) == WithdrawalNotice::kNone); // unknown before (load)
        {
            ComedownTimers t;
            CHECK(!IsHigh(t));
            OnComedownDose(t, DrugType::kDepressant, 10.0);
            CHECK(IsHigh(t));
            CancelComedowns(t);
            CHECK(!IsHigh(t));
        }
        {
            // "Shaky and Suspicious" applies to Velvet Haze and Dreamcap highs only, never to a Rimerush high alone.
            ComedownTimers t;
            CHECK(!IsShakyHigh(t));
            OnComedownDose(t, DrugType::kStimulant, 10.0);
            CHECK(IsHigh(t) && !IsShakyHigh(t));
            OnComedownDose(t, DrugType::kHallucinogen, 11.0);
            CHECK(IsShakyHigh(t));
            CancelComedowns(t);
            OnComedownDose(t, DrugType::kDepressant, 12.0);
            CHECK(IsShakyHigh(t));
        }
        CHECK(LostGearMessage("Windhelm") == "You wake up with a splitting headache. Last night is a blur... maybe the guards in Windhelm took your things.");
        CHECK(LostGearMessage("Windhelm").find("prison") == std::string::npos);

        // Did the teleport work? (compared after the settle delay)
        CHECK(kMoveSettleSeconds > 0.0 && kMoveMinUnits == 1000.0f);
        {
            const Point3 a{ 130992, 36118, -12271 };
            CHECK(!DidMove(a, a));                                   // the log's before == after: not moved
            CHECK(!DidMove(a, { 130992 + 999.0f, 36118, -12271 }));  // just under 1000
            CHECK(DidMove(a, { 130992 + 1000.0f, 36118, -12271 }));  // exactly 1000 counts
            CHECK(DidMove(a, { 0, 0, 0 }));
            CHECK(DidMove({ 0, 0, 0 }, { 600, 600, 600 }));          // 3D distance 1039
            CHECK(!DidMove(a, { std::numeric_limits<float>::quiet_NaN(), 0, 0 }));
            CHECK(!DidMove(a, { std::numeric_limits<float>::infinity(), 0, 0 }));
            const Point3 target{ 5000, 5000, 0 };
            CHECK(CheckMove(a, target, a) == MoveVerdict::kNotMoved);
            CHECK(CheckMove(a, target, target) == MoveVerdict::kMoved);
            CHECK(CheckMove(a, { a.x + 10, a.y, a.z }, a) == MoveVerdict::kNotApplicable);  // target next to the player
            CHECK(CheckMove(a, { a.x + 999, a.y, a.z }, a) == MoveVerdict::kNotApplicable);
            CHECK(CheckMove(a, { a.x + 1000, a.y, a.z }, a) == MoveVerdict::kNotMoved);
        }

        // Detect life recast and Lost Gear expiry.
        CHECK(kDetectLifeRecastSeconds == 55.0);
        CHECK(kLostGearHours == 72.0);
        CHECK(!LostGearExpired(100.0, 172.0));
        CHECK(LostGearExpired(172.0, 172.0));
        CHECK(LostGearExpired(200.0, 172.0));
        CHECK(LostGearExpired(100.0, 0.0));                                         // no expiry recorded
        CHECK(LostGearExpired(100.0, std::numeric_limits<double>::quiet_NaN()));
        CHECK(!LostGearExpired(std::numeric_limits<double>::quiet_NaN(), 172.0));  // unknown clock: keep it

        // Dose diagnostics: two log passes per dose (0.5 s and 5 s), each taken once.
        {
            DiagnosticTimers d{};
            CHECK(!TakeDueDiagnostic(d, 1000.0).has_value());
            ScheduleDiagnostics(d, DrugType::kHallucinogen, 100.0);
            CHECK(!TakeDueDiagnostic(d, 100.4).has_value());
            auto first = TakeDueDiagnostic(d, 100.5);
            CHECK(first.has_value() && first->pass == 1 && first->type == DrugType::kHallucinogen);
            CHECK(!TakeDueDiagnostic(d, 104.9).has_value());
            auto second = TakeDueDiagnostic(d, 105.0);
            CHECK(second.has_value() && second->pass == 2);
            CHECK(!TakeDueDiagnostic(d, 200.0).has_value());
        }

        // Comedown timers: due 120 s after a dose; a new dose restarts; cancel clears; never fires twice.
        {
            ComedownTimers t{};
            CHECK(!TakeDueComedown(t, 1000.0).has_value());
            OnComedownDose(t, DrugType::kStimulant, 100.0);
            CHECK(!TakeDueComedown(t, 219.9).has_value());
            OnComedownDose(t, DrugType::kStimulant, 200.0);              // restart
            CHECK(!TakeDueComedown(t, 319.9).has_value());
            CHECK(TakeDueComedown(t, 320.0) == std::optional<DrugType>(DrugType::kStimulant));
            CHECK(!TakeDueComedown(t, 400.0).has_value());               // consumed once
            OnComedownDose(t, DrugType::kDepressant, 0.0);
            OnComedownDose(t, DrugType::kHallucinogen, 10.0);
            CancelComedowns(t);
            CHECK(!TakeDueComedown(t, 10000.0).has_value());
            OnComedownDose(t, DrugType::kDepressant, 0.0);
            OnComedownDose(t, DrugType::kHallucinogen, 10.0);
            CHECK(TakeDueComedown(t, 500.0) == std::optional<DrugType>(DrugType::kDepressant));
            CHECK(TakeDueComedown(t, 500.0) == std::optional<DrugType>(DrugType::kHallucinogen));
            CHECK(!TakeDueComedown(t, 500.0).has_value());
            OnComedownDose(t, DrugType::kStimulant, std::numeric_limits<double>::quiet_NaN());  // ignored
            CHECK(!TakeDueComedown(t, 1e9).has_value());
        }

        // Withdrawal plan: unknown applied tier (-1) always changes; equal does nothing; tier 0 only removes.
        {
            auto p = PlanWithdrawal(0, 0);
            CHECK(!p.change);
            p = PlanWithdrawal(2, 2);
            CHECK(!p.change);
            p = PlanWithdrawal(0, 1);
            CHECK(p.change && p.addTier == 1);
            p = PlanWithdrawal(3, 1);
            CHECK(p.change && p.addTier == 1);
            p = PlanWithdrawal(2, 0);
            CHECK(p.change && p.addTier == 0);
            p = PlanWithdrawal(-1, 0);
            CHECK(p.change && p.addTier == 0);
            p = PlanWithdrawal(-1, 3);
            CHECK(p.change && p.addTier == 3);
            p = PlanWithdrawal(1, 99);  // out of range is clamped to 3
            CHECK(p.change && p.addTier == 3);
            p = PlanWithdrawal(1, -5);  // and negative to 0
            CHECK(p.change && p.addTier == 0);
        }

        // Blur pulse scheduling: 60-180 real seconds, only at tier >= 2 or with a hangover.
        {
            std::optional<double> due;
            CHECK(!TickBlurPulse(due, 100.0, 1, false, 0.5));       // not eligible
            CHECK(!due.has_value());
            CHECK(!TickBlurPulse(due, 100.0, 2, false, 0.0));       // eligible: schedules, no cast
            CHECK(due && *due == 160.0);
            CHECK(!TickBlurPulse(due, 159.9, 2, false, 0.5));
            CHECK(TickBlurPulse(due, 160.0, 2, false, 1.0));        // cast, reschedule 180 s out
            CHECK(due && *due == 340.0);
            CHECK(!TickBlurPulse(due, 200.0, 3, false, 0.5));
            CHECK(!TickBlurPulse(due, 200.0, 0, false, 0.5));       // dropped out: cleared
            CHECK(!due.has_value());
            CHECK(!TickBlurPulse(due, 300.0, 0, true, 0.5));        // hangover alone is eligible
            CHECK(due && *due == 300.0 + 120.0);
            CHECK(TickBlurPulse(due, 1000.0, 0, true, 0.25));       // long delay still casts once
            CHECK(due && *due == 1000.0 + 90.0);
            CHECK(!TickBlurPulse(due, 1001.0, 0, true, 0.5));
            CHECK(!TickBlurPulse(due, std::numeric_limits<double>::quiet_NaN(), 2, false, 0.5));  // bad clock ignored
            std::optional<double> d2;
            CHECK(!TickBlurPulse(d2, 0.0, 2, false, -3.0));         // rng clamped
            CHECK(d2 && *d2 == 60.0);
            d2.reset();
            CHECK(!TickBlurPulse(d2, 0.0, 2, false, 9.0));
            CHECK(d2 && *d2 == 180.0);
        }

        // Ragdoll intervals and scheduler.
        {
            CHECK(IntervalRangeForStack(-3).minSeconds == 30.0 && IntervalRangeForStack(0).maxSeconds == 80.0);
            CHECK(IntervalRangeForStack(1).minSeconds == 30.0 && IntervalRangeForStack(1).maxSeconds == 80.0);
            CHECK(IntervalRangeForStack(2).minSeconds == 20.0 && IntervalRangeForStack(2).maxSeconds == 55.0);
            CHECK(IntervalRangeForStack(3).minSeconds == 12.0 && IntervalRangeForStack(3).maxSeconds == 35.0);
            CHECK(IntervalRangeForStack(4).minSeconds == 8.0 && IntervalRangeForStack(4).maxSeconds == 22.0);
            CHECK(IntervalRangeForStack(99).minSeconds == 8.0);

            RagdollState s;
            OnRagdollDose(s, 100.0);
            CHECK(!TickRagdoll(s, 100.5, true, 1, 0.0));            // schedules 30 s out
            CHECK(s.nextDue && *s.nextDue == 130.5);
            CHECK(!TickRagdoll(s, 130.4, true, 1, 0.5));
            CHECK(TickRagdoll(s, 130.5, true, 1, 1.0));             // knock, next 80 s out
            CHECK(s.nextDue && *s.nextDue == 210.5);
            CHECK(!TickRagdoll(s, 131.0, true, 1, 0.5));
            CHECK(!TickRagdoll(s, 131.0, true, 2, 0.0));            // stack change reschedules, no cast
            CHECK(s.nextDue && *s.nextDue == 151.0);
            CHECK(TickRagdoll(s, 500.0, true, 2, 0.0));             // late still casts once
            CHECK(s.nextDue && *s.nextDue == 520.0);
            CHECK(!TickRagdoll(s, 501.0, true, 2, 0.0));
            CHECK(!TickRagdoll(s, 502.0, false, 2, 0.0));           // high ended: cleared
            CHECK(!s.nextDue.has_value());
            CHECK(!TickRagdoll(s, 600.0, false, 2, 0.0));
            // The post-dose quiet time: first knockdown never within 8 s of a dose, even with a tiny interval.
            RagdollState q;
            OnRagdollDose(q, 1000.0);
            CHECK(!TickRagdoll(q, 1000.0, true, 5, 0.0));           // 8 s interval from the dose
            CHECK(q.nextDue && *q.nextDue == 1008.0);
            CHECK(!TickRagdoll(q, 1007.9, true, 5, 0.0));
            CHECK(TickRagdoll(q, 1008.0, true, 5, 0.0));
            // Due but inside the post-dose time (a dose arrived after scheduling and nextDue was kept): stays due.
            RagdollState g;
            CHECK(!TickRagdoll(g, 0.0, true, 4, 0.0));
            g.lastDoseSeconds = 7.0;
            CHECK(!TickRagdoll(g, 8.0, true, 4, 0.0));              // due, but 1 s after the dose
            CHECK(g.nextDue && *g.nextDue == 8.0);
            CHECK(TickRagdoll(g, 15.0, true, 4, 0.0));
            // Minimum gap between knockdowns is 6 s.
            g.nextDue = 16.0;
            CHECK(!TickRagdoll(g, 16.0, true, 4, 0.0));             // only 1 s since the last knock
            CHECK(TickRagdoll(g, 21.0, true, 4, 0.0));
            // A dose during the high restarts the schedule.
            OnRagdollDose(g, 30.0);
            CHECK(!g.nextDue.has_value());
            CHECK(!TickRagdoll(g, 31.0, true, 4, 0.0));
            CHECK(g.nextDue && *g.nextDue == 39.0);
            CHECK(!TickRagdoll(g, std::numeric_limits<double>::quiet_NaN(), true, 4, 0.0));
            CHECK(TickRagdoll(g, 1e9, true, 4, std::numeric_limits<double>::quiet_NaN()));  // NaN rng -> 0.5, late knock
        }

        CHECK(NoteText("Dawnstar") == "Last night is a blur. Shouting. Guards. A cold floor, maybe a cell. Your things might be somewhere in Dawnstar... if the guards kept them.");
        CHECK(NoteText("Dawnstar").find("prison") == std::string::npos);
        CHECK(CapitalName("Whiterun") == "Whiterun" && CapitalName("Eastmarch") == "Windhelm" && CapitalName("Rift") == "Riften");
        CHECK(CapitalName("Haafingar") == "Solitude" && CapitalName("Hjaalmarch") == "Morthal" && CapitalName("Reach") == "Markarth");
        CHECK(CapitalName("Falkreath") == "Falkreath" && CapitalName("Pale") == "Dawnstar" && CapitalName("Winterhold") == "Winterhold");
        CHECK(CapitalName("Nowhere") == "Nowhere");
        return 0;
    }
}

int main()
{
    if (TestCore() != 0) return 1;
    namespace fs = std::filesystem;
    const fs::path data = fs::temp_directory_path() / "HighrimConfigTest" / "Data";
    fs::remove_all(data.parent_path());
    Highrim::Config::SetDataDirectoryForTests(data);

    // Built-in defaults when no files exist.
    Highrim::Config::Load();
    auto s = Highrim::Config::Get();
    CHECK(s.enabled && s.overdose && s.addiction && !s.debug);
    CHECK(s.doseWindowMinutes == 120);
    CHECK(s.blackoutDoses == 4);
    CHECK(s.ragdoll);
    CHECK(s.debugBlackoutKey == 0);
    CHECK(s.rimerushWithdrawalBlackout);  // default on
    CHECK(!s.fullBlackout);               // default OFF

    // Shipped defaults file.
    Write(data / "MCM/Config/Highrim/settings.ini",
        "[General]\nbEnabled = 1\nbOverdose = 1\nbAddiction = 0\niDoseWindowMinutes = 60\niBlackoutDoses = 6\nbDebug = 0\niDebugBlackoutKey = 0\n");
    Highrim::Config::Load();
    s = Highrim::Config::Get();
    CHECK(s.enabled && s.overdose && !s.addiction && !s.debug);
    CHECK(s.doseWindowMinutes == 60);
    CHECK(s.blackoutDoses == 6);

    // User override wins; values are clamped.
    Write(data / "MCM/Settings/Highrim.ini",
        "[General]\nbOverdose = 0\nbDebug = 1\niDoseWindowMinutes = 9999\niBlackoutDoses = 99\niDebugBlackoutKey = 24\nbFullBlackout = 1\nbRimerushWithdrawalBlackout = 0\n");
    Highrim::Config::Load();
    s = Highrim::Config::Get();
    CHECK(s.enabled);
    CHECK(!s.overdose);
    CHECK(!s.addiction);  // not overridden: shipped default still applies
    CHECK(s.debug);
    CHECK(s.doseWindowMinutes == 240);
    CHECK(s.blackoutDoses == 8);  // clamped
    CHECK(s.debugBlackoutKey == 24);
    CHECK(s.fullBlackout && !s.rimerushWithdrawalBlackout);

    fs::remove_all(data.parent_path());
    std::printf("ok\n");
    return 0;
}
