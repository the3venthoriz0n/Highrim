#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Pure rules: no game or SKSE headers, so everything here is unit tested.
namespace Highrim::Core
{
    enum class DrugType { kStimulant = 0, kDepressant = 1, kHallucinogen = 2 };

    // Stacking doses: each drug has its own stack. A dose of drug D inside the window (game minutes) of D's
    // previous dose adds one to D's stack; otherwise D's stack restarts at 1. Other drugs never touch it.
    inline constexpr int kMaxStack = 99;       // the count never grows past this
    inline constexpr int kMaxStackLevel = 3;   // extra effect levels on top of the potion's own
    struct DoseStacks
    {
        int    count[3]{};
        double lastHours[3]{ -1, -1, -1 };  // negative or non-finite = no usable previous dose
    };
    // Returns the drug's new stack (1 or more). An unknown drug returns 0 and changes nothing. A window below
    // 1, a clock that went backwards or a non-finite time restarts at 1.
    int  AddStackedDose(DoseStacks& s, DrugType type, double nowHours, int windowMinutes);
    void ResetStacks(DoseStacks& s);
    // True when the stack has reached the blackout dose count (a count below 2 is treated as 2).
    bool ShouldBlackout(int stack, int blackoutDoses);
    // 0 for stack 1 (the potion alone), 1..kMaxStackLevel for stacks 2 and up.
    int StackLevel(int stack);
    // Screen effect strength: 0.6 at stack 1, rising in equal steps to 1.0 at stack 1 + kMaxStackLevel.
    double StrengthForStack(int stack);

    // Combined counter for the blackout: doses of ANY drug inside the window of the previous dose of any drug.
    // Same rules as AddStackedDose (window below 1, clock going backwards or a non-finite time restarts at 1).
    struct DoseTotals
    {
        int    count{ 0 };
        double lastHours{ -1 };
    };
    int AddAnyDose(DoseTotals& t, double nowHours, int windowMinutes);

    // Velvet Haze screen effect strength: kVelvetStrengthBase at stack 1, plus kVelvetStrengthStep per extra stack, capped at 1.
    inline constexpr double kVelvetStrengthBase = 0.5;
    inline constexpr double kVelvetStrengthStep = 0.2;
    double StrengthForVelvetStack(int stack);

    // Addiction tuning, all in one place.
    inline constexpr float  kDoseAmount           = 30.f;  // level added per dose
    inline constexpr float  kMaxLevel             = 100.f;
    inline constexpr double kDecayPerDay          = 5.0;   // level lost per game day
    inline constexpr double kWithdrawalGraceHours = 4.0;   // no withdrawal this soon after the drug's last dose
    inline constexpr float  kTier1Level           = 10.f;  // tier 1 from here, tier 0 below
    inline constexpr float  kTier2Level           = 35.f;
    inline constexpr float  kTier3Level           = 60.f;

    struct Addiction
    {
        float  level[3]{};                      // 0 to 100
        double lastDoseHours[3]{ -1, -1, -1 };  // negative = never
        double lastUpdateHours{ 0 };
    };

    void Decay(Addiction& a, double nowHours);
    void AddDose(Addiction& a, DrugType type, double nowHours);

    int WithdrawalTier(float level, double hoursSinceLastDose);
    int HighestTier(const Addiction& a, double nowHours);

    std::string              Serialize(const Addiction& a);
    std::optional<Addiction> Deserialize(std::string_view text);


    // Maps the local form id of one of the three drugs to its type; anything else -> nullopt.
    std::optional<DrugType> DrugFromLocalId(std::uint32_t localId);

    // What the co-save keeps: addiction state plus the game hour at which the hangover ends (0 = none).
    struct PersistedState
    {
        Addiction addiction;
        double    hangoverExpiryHours{ 0 };
    };
    std::string                   SerializeState(const PersistedState& s);
    std::optional<PersistedState> DeserializeState(std::string_view text);  // strict; malformed -> nullopt

    // Comedown scheduling in real seconds: due kComedownDelaySeconds after the drug's last dose.
    inline constexpr double kComedownDelaySeconds = 120.0;
    struct ComedownTimers
    {
        std::array<std::optional<double>, 3> dueSeconds{};
    };
    void                    OnComedownDose(ComedownTimers& t, DrugType type, double nowSeconds);  // starts or restarts
    std::optional<DrugType> TakeDueComedown(ComedownTimers& t, double nowSeconds);  // returns one due drug and clears it
    void                    CancelComedowns(ComedownTimers& t);

    // Dose diagnostics: after a dose, the plugin logs the player's active effects at kDiagnosticFirstSeconds and
    // again at kDiagnosticSecondSeconds of real time. Each pending entry is taken once.
    inline constexpr double kDiagnosticFirstSeconds = 0.5;
    inline constexpr double kDiagnosticSecondSeconds = 5.0;
    struct DiagnosticEntry
    {
        DrugType type;
        double   dueSeconds;
        int      pass;  // 1 or 2
    };
    struct DiagnosticTimers
    {
        std::vector<DiagnosticEntry> pending;
    };
    void                           ScheduleDiagnostics(DiagnosticTimers& t, DrugType type, double nowSeconds);
    std::optional<DiagnosticEntry> TakeDueDiagnostic(DiagnosticTimers& t, double nowSeconds);

    // Withdrawal application. appliedTier is what the player currently carries (-1 = unknown, e.g. after a
    // load: always re-apply). When change is set, remove every Highrim withdrawal ability, then add the one for
    // addTier (0 = add none). wantedTier is clamped to 0..3.
    struct WithdrawalPlan
    {
        bool change{ false };
        int  addTier{ 0 };
    };
    WithdrawalPlan PlanWithdrawal(int appliedTier, int wantedTier);

    // Blur pulse: while tier >= 2 or the hangover is active, due every 60-180 real seconds (rng01 clamped to 0..1).
    // Returns true when the pulse should be cast now (and has already been rescheduled in nextDue). Not eligible
    // clears nextDue; eligible with no schedule schedules it without casting. Non-finite now is ignored.
    inline constexpr double kBlurMinSeconds = 60.0;
    inline constexpr double kBlurMaxSeconds = 180.0;
    bool TickBlurPulse(std::optional<double>& nextDue, double nowSeconds, int tier, bool hangover, double rng01);

    // Velvet Haze ragdolls: while the depressant high lasts, a knockdown is due at a random real-second interval
    // that shrinks with the depressant stack (1: 30-80, 2: 20-55, 3: 12-35, 4+: 8-22; below 1 counts as 1).
    // Never within kRagdollPostDoseSeconds of a dose, never twice within kRagdollMinGapSeconds.
    inline constexpr double kRagdollPostDoseSeconds = 8.0;
    inline constexpr double kRagdollMinGapSeconds   = 6.0;
    struct IntervalRange
    {
        double minSeconds;
        double maxSeconds;
    };
    IntervalRange IntervalRangeForStack(int stack);
    struct RagdollState
    {
        std::optional<double> nextDue;
        int                   stack{ 0 };  // the stack the schedule was made for; 0 = none
        double                lastDoseSeconds{ -1e18 };
        double                lastKnockSeconds{ -1e18 };
    };
    // A dose restarts the schedule (and the post-dose quiet time).
    void OnRagdollDose(RagdollState& s, double nowSeconds);
    // Returns true when a knockdown should happen now (already rescheduled). Not active clears the schedule.
    // Active with no schedule, or a changed stack, schedules from now without casting. A due knockdown that is
    // still inside the post-dose or minimum-gap quiet time stays due. Callers skip the call while a guard blocks
    // (so a postponement never consumes the interval) but still call it when not active. Non-finite now is ignored.
    bool TickRagdoll(RagdollState& s, double nowSeconds, bool active, int stack, double rng01);

    // Overdose deferral: a due overdose waits while a menu is open or the game is paused, and is dropped after
    // kOverdoseGiveUpSeconds of real time of waiting (counted from the due time). Non-finite times drop it.
    inline constexpr double kOverdoseGiveUpSeconds = 30.0;
    // A dose-triggered blackout knocks the player down first and starts the blackout this long after the knock.
    inline constexpr double kRagdollFirstSeconds = 2.5;
    inline double BlackoutStartAfterKnock(double knockSeconds) { return knockSeconds + kRagdollFirstSeconds; }
    enum class OverdoseAction { kWait, kRun, kDrop };
    OverdoseAction DecideOverdose(double nowSeconds, double dueSeconds, bool uiBlocked);

    struct Point { float x, y; };

    std::optional<std::size_t> NearestIndex(Point origin, std::span<const Point> points);
    std::optional<std::size_t> PickWakeSpot(std::span<const Point> candidates, Point origin,
        float minDist, float maxDist, float rng01);

    // Wake spot choice in stages, so a sparse area still yields a spot. Distances are measured from the prison's
    // anchor; candidates closer than kWakeNever are never used. Stage 1 15000-80000, stage 2 8000-150000,
    // stage 3 kWakeNever-unbounded. The first stage with any candidate decides; the pick inside it is
    // floor(rng01 * count) over the candidates in their given order (rng01 clamped to 0..1, NaN -> 0).
    inline constexpr float kWakeNever = 3000.0f;
    struct WakeSpotPick
    {
        std::size_t index;     // into the candidates span
        int         stage;     // 1, 2 or 3
        std::size_t inBand;    // how many candidates the stage had
        float       distance;  // of the pick from the origin
    };
    std::optional<WakeSpotPick> PickWakeSpotStaged(std::span<const Point> candidates, Point origin, float rng01);

    // Hold choice. anchor is the hold prison's position in Tamriel space. WorldHold says "a player in worldspace
    // `world` is in the capital of hold `hold`" (a hold can have several: its city worldspace and its castle).
    struct HoldInfo
    {
        bool  hasChest;
        Point anchor;
    };
    struct WorldHold
    {
        std::uint32_t world;
        std::size_t   hold;
    };
    enum class HoldReason { kCapitalWorld, kNearest };
    struct HoldChoice
    {
        std::size_t index;
        HoldReason  reason;
    };
    std::optional<std::size_t> DirectHoldForWorld(std::uint32_t world, std::span<const WorldHold> worlds, std::span<const HoldInfo> holds);
    // A player inside a hold's capital worldspace gets that hold; otherwise the nearest hold with a chest to
    // tamrielPos. nullopt when neither applies.
    std::optional<HoldChoice> ChooseHold(std::span<const HoldInfo> holds, std::span<const WorldHold> worlds,
        std::uint32_t playerWorld, std::optional<Point> tamrielPos);

    // Worldspace relations, as a plain id -> parent id map (0 or absent = no parent). Cycles are cut.
    using WorldParents = std::map<std::uint32_t, std::uint32_t>;
    std::uint32_t WorldRoot(std::uint32_t world, const WorldParents& parents);
    bool          IsDescendantWorld(std::uint32_t child, std::uint32_t ancestor, const WorldParents& parents);  // strict
    // How a prison anchor in jailWorld relates to the coordinate space (worldspace) used for comparison:
    // kSame: use the anchor as is (jailWorld 0 = unknown counts as same); kChild: jailWorld is a child worldspace
    // (a city), so use the capital's exterior position in the space instead; kUnrelated: skip the hold.
    enum class AnchorSpace { kSame, kChild, kUnrelated };
    AnchorSpace ClassifyAnchorSpace(std::uint32_t jailWorld, std::uint32_t space, const WorldParents& parents);

    std::string NoteText(std::string_view prisonName);
}
