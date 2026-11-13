#include "Core.h"
#include "Forms.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <vector>

namespace Highrim::Core
{
    namespace
    {
        constexpr float  kDoseAmount = 20.f;
        constexpr float  kMaxLevel = 100.f;
        constexpr double kDecayPerDay = 10.0;
        constexpr double kMinHoursForWithdrawal = 6.0;

        template <class T>
        bool ParseNumber(std::string_view token, T& out)
        {
            if (token.empty()) return false;
            T value{};
            const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (ec != std::errc{} || ptr != token.data() + token.size()) return false;
            if (!std::isfinite(value)) return false;
            out = value;
            return true;
        }

        template <class T>
        void Append(std::string& out, T value)
        {
            char buf[64];
            const auto res = std::to_chars(buf, buf + sizeof(buf), value);
            out.append(buf, res.ptr);
        }
    }

    bool IsOverdose(std::optional<double> lastDoseHours, double nowHours, int windowMinutes)
    {
        if (!lastDoseHours || windowMinutes < 1) return false;
        if (!std::isfinite(*lastDoseHours) || !std::isfinite(nowHours)) return false;
        const double elapsed = nowHours - *lastDoseHours;
        return elapsed >= 0.0 && elapsed <= static_cast<double>(windowMinutes) / 60.0;
    }

    void Decay(Addiction& a, double nowHours)
    {
        if (!std::isfinite(nowHours)) return;
        const double elapsed = nowHours - a.lastUpdateHours;
        if (!std::isfinite(elapsed) || elapsed < 0.0) return;
        const double drop = kDecayPerDay * (elapsed / 24.0);
        for (float& l : a.level) {
            l = static_cast<float>(std::max(0.0, static_cast<double>(l) - drop));
        }
        a.lastUpdateHours = nowHours;
    }

    void AddDose(Addiction& a, DrugType type, double nowHours)
    {
        const auto i = static_cast<std::size_t>(type);
        if (i >= 3) return;
        Decay(a, nowHours);
        a.level[i] = std::min(kMaxLevel, a.level[i] + kDoseAmount);
        a.lastDoseHours[i] = nowHours;
    }

    int WithdrawalTier(float level, double hoursSinceLastDose)
    {
        if (level < 10.f || hoursSinceLastDose < kMinHoursForWithdrawal) return 0;
        if (level < 40.f) return 1;
        if (level < 70.f) return 2;
        return 3;
    }

    int HighestTier(const Addiction& a, double nowHours)
    {
        Addiction copy = a;
        Decay(copy, nowHours);
        int best = 0;
        for (int i = 0; i < 3; ++i) {
            // Never dosed (negative): treat as long ago so the level alone decides.
            const double since = copy.lastDoseHours[i] < 0.0 ? 1e9 : nowHours - copy.lastDoseHours[i];
            best = std::max(best, WithdrawalTier(copy.level[i], since));
        }
        return best;
    }

    std::string Serialize(const Addiction& a)
    {
        std::string out = "HR1";
        for (float l : a.level) { out += ';'; Append(out, l); }
        for (double d : a.lastDoseHours) { out += ';'; Append(out, d); }
        out += ';';
        Append(out, a.lastUpdateHours);
        return out;
    }

    std::optional<Addiction> Deserialize(std::string_view text)
    {
        constexpr std::size_t kFields = 8;  // version + 3 levels + 3 dose times + last update
        std::string_view tokens[kFields];
        std::size_t count = 0;
        std::size_t start = 0;
        while (true) {
            const auto sep = text.find(';', start);
            if (count >= kFields) return std::nullopt;
            if (sep == std::string_view::npos) {
                tokens[count++] = text.substr(start);
                break;
            }
            tokens[count++] = text.substr(start, sep - start);
            start = sep + 1;
        }
        if (count != kFields || tokens[0] != "HR1") return std::nullopt;

        Addiction a{};
        for (int i = 0; i < 3; ++i) {
            if (!ParseNumber(tokens[1 + i], a.level[i])) return std::nullopt;
            if (a.level[i] < 0.f || a.level[i] > kMaxLevel) return std::nullopt;
            if (!ParseNumber(tokens[4 + i], a.lastDoseHours[i])) return std::nullopt;
        }
        if (!ParseNumber(tokens[7], a.lastUpdateHours)) return std::nullopt;
        return a;
    }

    std::optional<DrugType> DrugFromLocalId(std::uint32_t localId)
    {
        if (localId == Forms::HighrimRimerush) return DrugType::kStimulant;
        if (localId == Forms::HighrimVelvetHaze) return DrugType::kDepressant;
        if (localId == Forms::HighrimDreamcap) return DrugType::kHallucinogen;
        return std::nullopt;
    }

    std::string SerializeState(const PersistedState& s)
    {
        std::string out = "HS1|" + Serialize(s.addiction) + "|";
        Append(out, s.hangoverExpiryHours);
        return out;
    }

    std::optional<PersistedState> DeserializeState(std::string_view text)
    {
        constexpr std::string_view kTag = "HS1|";
        if (!text.starts_with(kTag)) return std::nullopt;
        text.remove_prefix(kTag.size());
        const auto sep = text.find('|');
        if (sep == std::string_view::npos) return std::nullopt;
        auto addiction = Deserialize(text.substr(0, sep));
        if (!addiction) return std::nullopt;
        PersistedState p{};
        p.addiction = *addiction;
        if (!ParseNumber(text.substr(sep + 1), p.hangoverExpiryHours)) return std::nullopt;
        if (p.hangoverExpiryHours < 0.0) return std::nullopt;
        return p;
    }

    void OnComedownDose(ComedownTimers& t, DrugType type, double nowSeconds)
    {
        const auto i = static_cast<std::size_t>(type);
        if (i >= 3 || !std::isfinite(nowSeconds)) return;
        t.dueSeconds[i] = nowSeconds + kComedownDelaySeconds;
    }

    std::optional<DrugType> TakeDueComedown(ComedownTimers& t, double nowSeconds)
    {
        if (!std::isfinite(nowSeconds)) return std::nullopt;
        for (std::size_t i = 0; i < 3; ++i) {
            if (t.dueSeconds[i] && nowSeconds >= *t.dueSeconds[i]) {
                t.dueSeconds[i].reset();
                return static_cast<DrugType>(i);
            }
        }
        return std::nullopt;
    }

    void CancelComedowns(ComedownTimers& t)
    {
        for (auto& d : t.dueSeconds) d.reset();
    }

    OverdoseAction DecideOverdose(double nowSeconds, double dueSeconds, bool uiBlocked)
    {
        if (!std::isfinite(nowSeconds) || !std::isfinite(dueSeconds)) return OverdoseAction::kDrop;
        if (nowSeconds < dueSeconds) return OverdoseAction::kWait;
        if (!uiBlocked) return OverdoseAction::kRun;
        return nowSeconds - dueSeconds >= kOverdoseGiveUpSeconds ? OverdoseAction::kDrop : OverdoseAction::kWait;
    }

    std::uint32_t WorldRoot(std::uint32_t world, const WorldParents& parents)
    {
        std::uint32_t cur = world;
        for (int i = 0; i < 64; ++i) {  // bounded: a cyclic map cannot hang us
            auto it = parents.find(cur);
            if (it == parents.end() || it->second == 0 || it->second == cur) return cur;
            cur = it->second;
        }
        return cur;
    }

    bool IsDescendantWorld(std::uint32_t child, std::uint32_t ancestor, const WorldParents& parents)
    {
        if (child == 0 || ancestor == 0 || child == ancestor) return false;
        std::uint32_t cur = child;
        for (int i = 0; i < 64; ++i) {
            auto it = parents.find(cur);
            if (it == parents.end() || it->second == 0 || it->second == cur) return false;
            cur = it->second;
            if (cur == ancestor) return true;
        }
        return false;
    }

    AnchorSpace ClassifyAnchorSpace(std::uint32_t jailWorld, std::uint32_t space, const WorldParents& parents)
    {
        if (jailWorld == 0 || space == 0 || jailWorld == space) return AnchorSpace::kSame;
        if (IsDescendantWorld(jailWorld, space, parents)) return AnchorSpace::kChild;
        return AnchorSpace::kUnrelated;
    }

    std::optional<std::size_t> NearestIndex(Point origin, std::span<const Point> points)
    {
        std::optional<std::size_t> best;
        float bestD2 = 0.f;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const float dx = points[i].x - origin.x;
            const float dy = points[i].y - origin.y;
            const float d2 = dx * dx + dy * dy;
            if (!best || d2 < bestD2) {  // strict: ties keep the lowest index
                best = i;
                bestD2 = d2;
            }
        }
        return best;
    }

    std::optional<std::size_t> PickWakeSpot(std::span<const Point> candidates, Point origin,
        float minDist, float maxDist, float rng01)
    {
        std::vector<std::size_t> band;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const float dist = std::hypot(candidates[i].x - origin.x, candidates[i].y - origin.y);
            if (dist >= minDist && dist <= maxDist) band.push_back(i);
        }
        if (band.empty()) return std::nullopt;
        if (!(rng01 > 0.f)) rng01 = 0.f;  // also catches NaN
        if (rng01 > 1.f) rng01 = 1.f;
        auto pick = static_cast<std::size_t>(std::floor(static_cast<double>(rng01) * static_cast<double>(band.size())));
        pick = std::min(pick, band.size() - 1);
        return band[pick];
    }

    std::string NoteText(std::string_view prisonName)
    {
        std::string out = "Your things are in ";
        out += prisonName;
        out += '.';
        return out;
    }

    WithdrawalPlan PlanWithdrawal(int appliedTier, int wantedTier)
    {
        const int wanted = std::clamp(wantedTier, 0, 3);
        if (appliedTier == wanted) return {};
        return { true, wanted };
    }

    bool TickBlurPulse(std::optional<double>& nextDue, double nowSeconds, int tier, bool hangover, double rng01)
    {
        if (!std::isfinite(nowSeconds)) return false;
        if (tier < 2 && !hangover) {
            nextDue.reset();
            return false;
        }
        const double r        = std::isfinite(rng01) ? std::clamp(rng01, 0.0, 1.0) : 0.5;
        const double interval = kBlurMinSeconds + r * (kBlurMaxSeconds - kBlurMinSeconds);
        if (!nextDue) {
            nextDue = nowSeconds + interval;
            return false;
        }
        if (nowSeconds < *nextDue) return false;
        nextDue = nowSeconds + interval;
        return true;
    }
}
