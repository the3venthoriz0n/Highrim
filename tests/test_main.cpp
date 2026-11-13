#include "Config.h"
#include "Core.h"
#include "Forms.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <filesystem>
#include <fstream>
#include <string>

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

        // IsOverdose: window 30 min = 0.5 h.
        CHECK(!IsOverdose(std::nullopt, 10.0, 30));
        CHECK(IsOverdose(10.0, 10.0, 30));                 // same instant
        CHECK(IsOverdose(10.0, 10.25, 30));
        CHECK(IsOverdose(10.0, 10.5, 30));                 // exactly the window
        CHECK(!IsOverdose(10.0, 10.5 + 1.0 / 3600, 30));   // one second over
        CHECK(!IsOverdose(10.0, 9.99, 30));                // clock went backwards
        CHECK(!IsOverdose(nan, 10.0, 30));
        CHECK(!IsOverdose(10.0, nan, 30));
        CHECK(!IsOverdose(10.0, inf, 30));
        CHECK(!IsOverdose(inf, 10.0, 30));
        CHECK(!IsOverdose(10.0, 10.0, 0));
        CHECK(!IsOverdose(10.0, 10.0, -5));
        CHECK(IsOverdose(10.0, 10.0, 1));

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
            Decay(a, 24.0);  // one day
            CHECK(std::fabs(a.level[0] - 40.f) < 1e-3f);
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
            CHECK(a.level[1] == 20.f);
            CHECK(a.lastDoseHours[1] == 5.0);
            CHECK(a.lastDoseHours[0] < 0.0 && a.lastDoseHours[2] < 0.0);
            CHECK(a.lastUpdateHours == 5.0);
            AddDose(a, DrugType::kDepressant, 5.0 + 24.0);  // decays 10 first, then +20
            CHECK(std::fabs(a.level[1] - 30.f) < 1e-3f);
            for (int i = 0; i < 10; ++i) AddDose(a, DrugType::kDepressant, 29.0);
            CHECK(a.level[1] == 100.f);
            AddDose(a, DrugType::kHallucinogen, 29.0);
            CHECK(a.level[2] == 20.f);
        }

        // WithdrawalTier.
        CHECK(WithdrawalTier(9.9f, 100.0) == 0);
        CHECK(WithdrawalTier(10.f, 100.0) == 1);
        CHECK(WithdrawalTier(39.9f, 100.0) == 1);
        CHECK(WithdrawalTier(40.f, 100.0) == 2);
        CHECK(WithdrawalTier(69.9f, 100.0) == 2);
        CHECK(WithdrawalTier(70.f, 100.0) == 3);
        CHECK(WithdrawalTier(100.f, 100.0) == 3);
        CHECK(WithdrawalTier(70.f, 5.99) == 0);
        CHECK(WithdrawalTier(70.f, 6.0) == 3);

        // HighestTier decays a copy and does not mutate.
        {
            Addiction a = fresh();
            a.level[0] = 15.f; a.lastDoseHours[0] = 0.0;
            a.level[2] = 75.f; a.lastDoseHours[2] = 0.0;
            CHECK(HighestTier(a, 6.0) == 3);
            CHECK(a.level[2] == 75.f && a.lastUpdateHours == 0.0);
            CHECK(HighestTier(a, 5.0) == 0);
            Addiction b = fresh();
            b.level[1] = 50.f;  // never dosed (negative): treated as long ago, tier applies
            CHECK(HighestTier(b, 1.0) == 2);
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

        CHECK(NoteText("Dawnstar Sanctuary") == "Your things are in Dawnstar Sanctuary.");
        CHECK(NoteText("") == "Your things are in .");
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
    CHECK(s.doseWindowMinutes == 30);
    CHECK(s.debugBlackoutKey == 0);

    // Shipped defaults file.
    Write(data / "MCM/Config/Highrim/settings.ini",
        "[General]\nbEnabled = 1\nbOverdose = 1\nbAddiction = 0\niDoseWindowMinutes = 60\nbDebug = 0\niDebugBlackoutKey = 0\n");
    Highrim::Config::Load();
    s = Highrim::Config::Get();
    CHECK(s.enabled && s.overdose && !s.addiction && !s.debug);
    CHECK(s.doseWindowMinutes == 60);

    // User override wins; values are clamped.
    Write(data / "MCM/Settings/Highrim.ini",
        "[General]\nbOverdose = 0\nbDebug = 1\niDoseWindowMinutes = 9999\niDebugBlackoutKey = 24\n");
    Highrim::Config::Load();
    s = Highrim::Config::Get();
    CHECK(s.enabled);
    CHECK(!s.overdose);
    CHECK(!s.addiction);  // not overridden: shipped default still applies
    CHECK(s.debug);
    CHECK(s.doseWindowMinutes == 240);
    CHECK(s.debugBlackoutKey == 24);

    fs::remove_all(data.parent_path());
    std::printf("ok\n");
    return 0;
}
