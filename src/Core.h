#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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

    struct Point { float x, y; };

    std::optional<std::size_t> NearestIndex(Point origin, std::span<const Point> points);
    std::optional<std::size_t> PickWakeSpot(std::span<const Point> candidates, Point origin,
        float minDist, float maxDist, float rng01);

    std::string NoteText(std::string_view prisonName);
}
