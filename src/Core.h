#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Pure rules: no game or SKSE headers, so everything here is unit tested.
namespace Highrim::Core
{
    enum class DrugType { kStimulant = 0, kDepressant = 1, kHallucinogen = 2 };

    // True when a last dose exists and 0 <= now - last <= windowMinutes / 60.
    bool IsOverdose(std::optional<double> lastDoseHours, double nowHours, int windowMinutes);

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

    // Overdose deferral: a due overdose waits while a menu is open or the game is paused, and is dropped after
    // kOverdoseGiveUpSeconds of real time of waiting (counted from the due time). Non-finite times drop it.
    inline constexpr double kOverdoseGiveUpSeconds = 30.0;
    enum class OverdoseAction { kWait, kRun, kDrop };
    OverdoseAction DecideOverdose(double nowSeconds, double dueSeconds, bool uiBlocked);

    struct Point { float x, y; };

    std::optional<std::size_t> NearestIndex(Point origin, std::span<const Point> points);
    std::optional<std::size_t> PickWakeSpot(std::span<const Point> candidates, Point origin,
        float minDist, float maxDist, float rng01);

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
