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
        }

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
