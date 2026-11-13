#include "PCH.h"

#include "Blackout.h"

#include "Config.h"
#include "Core.h"
#include "Doses.h"
#include "Forms.h"
#include "Prison.h"
#include "Withdrawal.h"

#include <cmath>
#include <random>
#include <thread>
#include <vector>

namespace Highrim::Blackout
{
    namespace
    {
        constexpr auto kPollInterval = std::chrono::milliseconds(100);
        constexpr auto kFadeOutTime  = std::chrono::milliseconds(1500);
        constexpr auto kBlackTime    = std::chrono::milliseconds(2000);
        constexpr float kMinWakeDistance = 8000.0f;
        constexpr float kMaxWakeDistance = 30000.0f;

        // Vanilla image space modifiers (Skyrim.esm, confirmed by parsing it).
        constexpr RE::FormID kFadeToBlack     = 0x000F756D;
        constexpr RE::FormID kFadeToBlackHold = 0x000F756E;
        constexpr RE::FormID kFadeToBlackBack = 0x000F756F;
        constexpr RE::FormID kTamriel         = 0x0000003C;

        std::atomic_bool g_started{ false };
        std::atomic_bool g_taskPending{ false };

        // All of these are touched on the game thread only (the expiry is also read by Task 5's save callbacks,
        // which run on the game thread too).
        double g_hangoverExpiry = 0.0;

        enum class Phase { kIdle, kFadingOut, kBlack };
        std::atomic<Phase>                    g_phase{ Phase::kIdle };
        std::chrono::steady_clock::time_point g_deadline;
        std::string                           g_reason;

        // Bumped on load, new game, revert and player death. A blackout remembers the value it started with.
        std::atomic<std::uint32_t> g_generation{ 0 };
        std::uint32_t              g_runGeneration = 0;
        std::atomic_bool           g_needCleanup{ false };  // a load interrupted a running blackout
        bool                       g_wasDead = false;

        void BumpGeneration(const char* why)
        {
            const auto g = ++g_generation;
            spdlog::info("Blackout: generation -> {} ({})", g, why);
        }

        bool MenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return true;
            }
            if (ui->GameIsPaused() || ui->IsItemMenuOpen() || ui->IsModalMenuOpen()) {
                return true;
            }
            for (std::string_view name : { RE::InventoryMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME, RE::FavoritesMenu::MENU_NAME,
                     RE::MagicMenu::MENU_NAME, RE::BarterMenu::MENU_NAME, RE::GiftMenu::MENU_NAME, RE::CraftingMenu::MENU_NAME,
                     RE::SleepWaitMenu::MENU_NAME, RE::TweenMenu::MENU_NAME, RE::MapMenu::MENU_NAME, RE::JournalMenu::MENU_NAME,
                     RE::StatsMenu::MENU_NAME, RE::BookMenu::MENU_NAME, RE::LockpickingMenu::MENU_NAME,
                     RE::MessageBoxMenu::MENU_NAME, RE::DialogueMenu::MENU_NAME, RE::Console::MENU_NAME,
                     RE::LoadingMenu::MENU_NAME, RE::MainMenu::MENU_NAME, RE::TrainingMenu::MENU_NAME }) {
                if (ui->IsMenuOpen(name)) {
                    return true;
                }
            }
            return false;
        }

        // Empty when the blackout may go on; otherwise why not. Used right before the steps that change the game.
        std::string GuardFailure(RE::PlayerCharacter* player, std::uint32_t generation)
        {
            if (generation != g_generation.load()) {
                return "the game was loaded or reverted";
            }
            if (!player) {
                return "no player";
            }
            if (player->IsDead()) {
                return "the player died";
            }
            if (!player->Is3DLoaded() || !player->GetParentCell()) {
                return "the player is not loaded";
            }
            if (!RE::ControlMap::GetSingleton()) {
                return "no control map";
            }
            if (MenuOpen()) {
                return "a menu is open or the game is paused";
            }
            return {};
        }

        constexpr RE::UserEvents::USER_EVENT_FLAG kBlocked = static_cast<RE::UserEvents::USER_EVENT_FLAG>(
            static_cast<int>(RE::UserEvents::USER_EVENT_FLAG::kMovement) |
            static_cast<int>(RE::UserEvents::USER_EVENT_FLAG::kLooking) |
            static_cast<int>(RE::UserEvents::USER_EVENT_FLAG::kActivate) |
            static_cast<int>(RE::UserEvents::USER_EVENT_FLAG::kMenu) |
            static_cast<int>(RE::UserEvents::USER_EVENT_FLAG::kFighting) |
            static_cast<int>(RE::UserEvents::USER_EVENT_FLAG::kSneaking) |
            static_cast<int>(RE::UserEvents::USER_EVENT_FLAG::kJumping));

        void SetControls(bool enable)
        {
            if (auto* map = RE::ControlMap::GetSingleton()) {
                map->ToggleControls(kBlocked, enable, true);
                spdlog::info("Blackout: player controls {}", enable ? "enabled" : "disabled");
            } else {
                spdlog::warn("Blackout: no ControlMap, controls not changed");
            }
        }

        void TriggerImod(RE::FormID id, const char* what)
        {
            auto* imod = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(id);
            if (!imod) {
                spdlog::warn("Blackout: image space modifier {} ({:08X}) not found, no visual fade", what, id);
                return;
            }
            RE::ImageSpaceModifierInstanceForm::Trigger(imod, 1.0f, nullptr);
            spdlog::info("Blackout: applied {}", what);
        }

        void StopImod(RE::FormID id, const char* what)
        {
            if (auto* imod = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(id)) {
                RE::ImageSpaceModifierInstanceForm::Stop(imod);
                spdlog::info("Blackout: stopped {}", what);
            }
        }

        double NowHours()
        {
            auto* calendar = RE::Calendar::GetSingleton();
            if (!calendar || !calendar->gameDaysPassed) {
                return -1.0;
            }
            return static_cast<double>(calendar->gameDaysPassed->value) * 24.0;
        }

        RE::SpellItem* HangoverSpell()
        {
            auto* handler = RE::TESDataHandler::GetSingleton();
            return handler ? handler->LookupForm<RE::SpellItem>(Forms::HighrimHangover, Forms::kPluginName) : nullptr;
        }

        struct Moved
        {
            RE::TESBoundObject* object;
            std::int32_t        count;
        };

        std::int32_t CountIn(RE::TESObjectREFR* ref, RE::TESBoundObject* obj)
        {
            auto counts = ref->GetInventoryCounts([obj](RE::TESBoundObject& o) { return &o == obj; });
            auto it     = counts.find(obj);
            return it == counts.end() ? 0 : it->second;
        }

        // Puts every moved item back from the chest to the player. Logs each item that does not come back and
        // returns how many stacks could not be returned in full.
        int Rollback(RE::PlayerCharacter* player, RE::TESObjectREFR* chest, const std::vector<Moved>& moved)
        {
            spdlog::warn("Blackout: rolling back {} item stack(s) from the chest to the player", moved.size());
            int missing = 0;
            for (const auto& m : moved) {
                try {
                    const auto inChest = CountIn(chest, m.object);
                    if (inChest < m.count) {
                        ++missing;
                    }
                    if (inChest <= 0) {
                        spdlog::error("Blackout: rollback: {:08X} x{} is no longer in the chest", m.object->GetFormID(), m.count);
                        continue;
                    }
                    chest->RemoveItem(m.object, std::min(inChest, m.count), RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
                } catch (...) {
                    ++missing;
                    spdlog::error("Blackout: rollback: exception on {:08X}", m.object->GetFormID());
                }
            }
            if (missing > 0) {
                spdlog::error("Blackout: rollback: {} stack(s) did NOT come back in full", missing);
            }
            return missing;
        }

        void Finish(bool success, int missing = 0)
        {
            StopImod(kFadeToBlackHold, "FadeToBlackHold");
            TriggerImod(kFadeToBlackBack, "FadeToBlackBack");
            SetControls(true);
            if (success) {
                auto* p = RE::PlayerCharacter::GetSingleton();
                if (p && !p->IsDead()) {
                    RE::SendHUDMessage::ShowHUDMessage("You wake up with a splitting headache.");
                }
            }
            if (success) {
                spdlog::info("Blackout: finished (success)");
            } else if (missing > 0) {
                spdlog::error("Blackout: finished (aborted; {} item stack(s) could NOT be returned from the chest)", missing);
            } else {
                spdlog::info("Blackout: finished (aborted, items untouched or all returned)");
            }
            g_phase = Phase::kIdle;
        }

        // missing: stacks a rollback could not return (0 when nothing was moved or everything came back).
        void Abort(const std::string& why, bool showBox = true, int missing = 0)
        {
            spdlog::error("Blackout: ABORT: {}{}", why, missing > 0 ? " (some items are still in the prison chest)" : "");
            if (showBox) {
                if (missing > 0) {
                    RE::DebugMessageBox(("Highrim: the blackout was cancelled, but " + std::to_string(missing) +
                        " item stack(s) could not be returned from the prison chest and may still be in it. See Highrim.log. (" + why + ")").c_str());
                } else {
                    RE::DebugMessageBox(("Highrim: the blackout was cancelled and nothing was taken from you. (" + why + ")").c_str());
                }
            }
            Finish(false, missing);
        }

        // Steps 2 to 6 of the sequence, run once the screen is black. Game thread.
        void DoWork()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (const auto why = GuardFailure(player, g_runGeneration); !why.empty()) {
                Abort("cancelled before anything moved: " + why, !player || !player->IsDead());
                return;
            }
            std::vector<Moved>  moved;
            RE::TESObjectREFR*  chest = nullptr;
            try {
                // Step 2: the hold whose prison is nearest the player.
                spdlog::info("Blackout: step 2: choosing the nearest hold prison");
                std::optional<Prison::Anchor> here = Prison::ExteriorAnchor(player);
                if (here) {
                    spdlog::info("Blackout: step 2: player anchor ({:.0f},{:.0f}) from {}", here->x, here->y, here->how);
                } else {
                    const auto ext = player->GetPlayerRuntimeData().exteriorPosition;
                    here           = Prison::Anchor{ ext.x, ext.y, nullptr, "player last exteriorPosition" };
                    spdlog::info("Blackout: step 2: player anchor ({:.0f},{:.0f}) from {} (worldspace unknown)", ext.x, ext.y, here->how);
                }

                auto* tamriel = RE::TESForm::LookupByID<RE::TESWorldSpace>(kTamriel);
                RE::TESWorldSpace* playerWorld = here->world ? here->world : tamriel;

                // Parent chains of every worldspace involved, for the "same space / child of" decisions.
                Core::WorldParents parents;
                auto addChain = [&](RE::TESWorldSpace* w) {
                    for (int i = 0; w && i < 16; ++i) {
                        auto* p = w->parentWorld;
                        parents[w->GetFormID()] = p ? p->GetFormID() : 0u;
                        w = p;
                    }
                };
                addChain(playerWorld);

                struct Candidate
                {
                    Prison::Resolved r;
                    Prison::Anchor   anchor;
                };
                struct Pending
                {
                    Prison::Resolved              r;
                    std::optional<Prison::Anchor> jailAnchor;
                };
                std::vector<Pending> pending;
                for (const auto& def : Prison::Holds()) {
                    auto r = Prison::Resolve(def);
                    if (!r.chest) {
                        spdlog::info("Blackout: step 2: {} skipped, no confiscation chest", def.name);
                        continue;
                    }
                    std::optional<Prison::Anchor> a = r.jail ? Prison::ExteriorAnchor(r.jail) : std::nullopt;
                    if (a) {
                        addChain(a->world);
                    }
                    pending.push_back({ std::move(r), std::move(a) });
                }

                // The space to compare in: the root of the player's worldspace (normally Tamriel).
                const std::uint32_t playerWorldId = playerWorld ? playerWorld->GetFormID() : 0u;
                const std::uint32_t spaceId       = Core::WorldRoot(playerWorldId, parents);
                auto* space = spaceId == playerWorldId ? playerWorld : RE::TESForm::LookupByID<RE::TESWorldSpace>(spaceId);
                if (spaceId != playerWorldId) {
                    // The player is inside a city worldspace: stand in for them with the capital's marker in the parent space.
                    std::optional<Prison::Anchor> mapped;
                    for (const auto& p : pending) {
                        if (p.jailAnchor && p.jailAnchor->world == playerWorld) {
                            for (const auto& m : Prison::WorldMarkers(space)) {
                                if (_stricmp(m.name.c_str(), p.r.def->capital) == 0) {
                                    mapped = Prison::Anchor{ m.x, m.y, space,
                                        std::string("map marker of ") + p.r.def->capital + " in the parent worldspace" };
                                    break;
                                }
                            }
                            break;
                        }
                    }
                    if (mapped) {
                        spdlog::info("Blackout: step 2: player is in worldspace {:08X}, a child of {:08X}; using {} at ({:.0f},{:.0f})",
                            playerWorldId, spaceId, mapped->how, mapped->x, mapped->y);
                        here = std::move(mapped);
                    } else {
                        spdlog::warn("Blackout: step 2: player is in child worldspace {:08X} but no capital marker was found in {:08X}; "
                                     "comparing inside the child worldspace only", playerWorldId, spaceId);
                        space = playerWorld;
                    }
                }
                RE::TESWorldSpace* world   = space ? space : tamriel;
                const std::uint32_t worldId = world ? world->GetFormID() : 0u;
                auto                markers = Prison::WorldMarkers(world);
                spdlog::info("Blackout: step 2: comparing in worldspace {:08X}, which has {} map markers", worldId, markers.size());

                std::vector<Candidate>   candidates;
                std::vector<Core::Point> points;
                for (auto& p : pending) {
                    const auto& def           = *p.r.def;
                    auto        capitalMarker = [&](const std::string& why) -> std::optional<Prison::Anchor> {
                        for (const auto& m : markers) {
                            if (_stricmp(m.name.c_str(), def.capital) == 0) {
                                return Prison::Anchor{ m.x, m.y, world, std::string("map marker of ") + def.capital + " (" + why + ")" };
                            }
                        }
                        return std::nullopt;
                    };
                    std::optional<Prison::Anchor> a;
                    if (!p.jailAnchor) {
                        a = capitalMarker("fallback, no jail anchor");
                        if (!a) {
                            spdlog::info("Blackout: step 2: {} skipped, no exterior anchor for its prison", def.name);
                            continue;
                        }
                    } else {
                        const auto jailWorldId = p.jailAnchor->world ? p.jailAnchor->world->GetFormID() : 0u;
                        switch (Core::ClassifyAnchorSpace(jailWorldId, worldId, parents)) {
                        case Core::AnchorSpace::kSame:
                            a = p.jailAnchor;
                            break;
                        case Core::AnchorSpace::kChild:
                            a = capitalMarker(std::format("jail is in city worldspace {:08X}, capital marker used", jailWorldId));
                            if (!a) {
                                spdlog::info("Blackout: step 2: {} skipped, jail is in city worldspace {:08X} and the capital has no marker in {:08X}",
                                    def.name, jailWorldId, worldId);
                                continue;
                            }
                            break;
                        default:
                            spdlog::info("Blackout: step 2: {} skipped, prison anchor is in unrelated worldspace {:08X}", def.name, jailWorldId);
                            continue;
                        }
                    }
                    spdlog::info("Blackout: step 2: candidate {} anchor ({:.0f},{:.0f}) via {}", def.name, a->x, a->y, a->how);
                    points.push_back({ a->x, a->y });
                    candidates.push_back({ std::move(p.r), std::move(*a) });
                }
                const auto nearest = Core::NearestIndex({ here->x, here->y }, points);
                if (!nearest) {
                    Abort("no hold prison with a chest could be located");
                    return;
                }
                const auto& hold = candidates[*nearest];
                chest            = hold.r.chest;
                spdlog::info("Blackout: step 2: chose {} (chest {:08X} via {})", hold.r.def->name, chest->GetFormID(), hold.r.chestSource);

                // Step 5 (done before any item moves, so a failure here costs nothing): a wake spot.
                spdlog::info("Blackout: step 5: picking a wake spot {:.0f} to {:.0f} units from the prison", kMinWakeDistance, kMaxWakeDistance);
                std::vector<Core::Point>        spotPoints;
                std::vector<RE::TESObjectREFR*> spotRefs;
                for (const auto& m : markers) {
                    if (m.visible && m.ref) {
                        spotPoints.push_back({ m.x, m.y });
                        spotRefs.push_back(m.ref);
                    }
                }
                std::string spotSource = "persistent-cell map markers (visible flag)";
                spdlog::info("Blackout: step 5: {} visible markers from the worldspace persistent cell", spotRefs.size());
                if (spotRefs.empty()) {
                    spotSource = "player currentMapMarkers";
                    for (auto& handle : player->GetPlayerRuntimeData().currentMapMarkers) {
                        if (auto ref = handle.get()) {
                            auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
                            if (extra) {
                                const auto pos = ref->GetPosition();
                                spotPoints.push_back({ pos.x, pos.y });
                                spotRefs.push_back(ref.get());
                            }
                        }
                    }
                    spdlog::info("Blackout: step 5: {} markers from player currentMapMarkers", spotRefs.size());
                }
                static std::mt19937 rng{ std::random_device{}() };
                const float         roll = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
                const auto spot = Core::PickWakeSpot(spotPoints, { hold.anchor.x, hold.anchor.y }, kMinWakeDistance, kMaxWakeDistance, roll);
                if (!spot) {
                    Abort("no wake spot between 8000 and 30000 units from the prison");
                    return;
                }
                RE::TESObjectREFR* spotRef = spotRefs[*spot];
                spdlog::info("Blackout: step 5: wake spot marker {:08X} at ({:.0f},{:.0f}) from {}", spotRef->GetFormID(),
                    spotPoints[*spot].x, spotPoints[*spot].y, spotSource);

                // Step 3 and 4: the chest, then the items.
                spdlog::info("Blackout: step 3: using chest {:08X} ({})", chest->GetFormID(), hold.r.def->name);
                auto chestBefore = chest->GetInventoryCounts();
                spdlog::info("Blackout: step 3: chest holds {} item types before", chestBefore.size());

                if (const auto why = GuardFailure(player, g_runGeneration); !why.empty()) {
                    Abort("cancelled before step 4, nothing moved: " + why, !player->IsDead());
                    return;
                }
                spdlog::info("Blackout: step 4: unequipping worn items");
                auto* equip = RE::ActorEquipManager::GetSingleton();
                if (equip) {
                    auto inv = player->GetInventory();
                    for (auto& [obj, data] : inv) {
                        if (obj && data.second && data.second->IsWorn() && !data.second->IsQuestObject()) {
                            const bool ok = equip->UnequipObject(player, obj, nullptr, static_cast<std::uint32_t>(std::max(1, data.first)),
                                nullptr, false, true, false, true);
                            spdlog::info("Blackout: step 4: unequip {:08X} ({}) -> {}", obj->GetFormID(), obj->GetName(), ok);
                        }
                    }
                } else {
                    spdlog::warn("Blackout: step 4: no ActorEquipManager, items stay equipped (they are moved anyway)");
                }

                auto inventory = player->GetInventory();
                spdlog::info("Blackout: step 4: {} item types in the player inventory", inventory.size());
                std::int32_t keptQuest = 0;
                std::int32_t skippedNonPlayable = 0;
                for (auto& [obj, data] : inventory) {
                    if (!obj || data.first <= 0) {
                        continue;
                    }
                    if (data.second && data.second->IsQuestObject()) {
                        spdlog::info("Blackout: step 4: keeping quest item {:08X} ({})", obj->GetFormID(), obj->GetName());
                        ++keptQuest;
                        continue;
                    }
                    const bool nonPlayable = (obj->GetFormFlags() & static_cast<std::uint32_t>(RE::TESForm::RecordFlags::kNonPlayable)) != 0;
                    const auto* weapon     = obj->As<RE::TESObjectWEAP>();
                    if (nonPlayable || (weapon && weapon->IsBound())) {
                        spdlog::info("Blackout: step 4: skipping non-playable {:08X} ({}) x{}{}", obj->GetFormID(), obj->GetName(), data.first,
                            (weapon && weapon->IsBound()) ? " (bound weapon)" : "");
                        ++skippedNonPlayable;
                        continue;
                    }
                    const std::int32_t count        = data.first;
                    const auto         chestPrev    = CountIn(chest, obj);
                    moved.push_back({ obj, count });
                    // RemoveItem with a target moves the whole stack with its extra data (enchant charge, tempering).
                    player->RemoveItem(obj, count, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, chest);
                    const auto chestNow  = CountIn(chest, obj);
                    const auto playerNow = CountIn(player, obj);
                    if (chestNow - chestPrev != count || playerNow != 0) {
                        spdlog::error("Blackout: step 4: MISMATCH for {:08X} ({}): moved {}, chest {} -> {}, player now {}",
                            obj->GetFormID(), obj->GetName(), count, chestPrev, chestNow, playerNow);
                        const int missing = Rollback(player, chest, moved);
                        Abort("item transfer did not verify", true, missing);
                        return;
                    }
                }
                spdlog::info("Blackout: step 4: moved {} item stack(s) to the chest, verified; kept {} quest item(s), skipped {} non-playable", moved.size(), keptQuest,
                    skippedNonPlayable);

                // Before step 6: re-check. Anything wrong now puts the items back and aborts.
                if (const auto why = GuardFailure(player, g_runGeneration); !why.empty()) {
                    spdlog::error("Blackout: before step 6: {}, rolling back", why);
                    const int missing = Rollback(player, chest, moved);
                    Abort("cancelled before step 6: " + why, !player->IsDead(), missing);
                    return;
                }

                // Step 6: teleport, note, hangover, time.
                spdlog::info("Blackout: step 6: teleporting to marker {:08X}", spotRef->GetFormID());
                const auto before = player->GetPosition();
                player->MoveTo(spotRef);
                const auto after = player->GetPosition();
                spdlog::info("Blackout: step 6: position ({:.0f},{:.0f},{:.0f}) -> ({:.0f},{:.0f},{:.0f})", before.x, before.y, before.z,
                    after.x, after.y, after.z);

                auto* handler = RE::TESDataHandler::GetSingleton();
                auto* note    = handler ? handler->LookupForm<RE::TESBoundObject>(hold.r.def->noteLocalId, Forms::kPluginName) : nullptr;
                if (note) {
                    player->AddObjectToContainer(note, nullptr, 1, nullptr);
                    spdlog::info("Blackout: step 6: note given ({}), player now has {}; text: {}", note->GetName(), CountIn(player, note),
                        Core::NoteText(std::string(hold.r.def->name) + " prison"));
                } else {
                    spdlog::error("Blackout: step 6: note for {} not found in {}", hold.r.def->name, Forms::kPluginName);
                }

                if (auto* calendar = RE::Calendar::GetSingleton(); calendar && calendar->gameHour && calendar->gameDaysPassed) {
                    const float hourBefore = calendar->gameHour->value;
                    float       hour       = hourBefore + 6.0f;
                    if (hour >= 24.0f) {
                        hour -= 24.0f;
                    }
                    calendar->gameHour->value = hour;
                    calendar->gameDaysPassed->value += 0.25f;
                    spdlog::info("Blackout: step 6: game time advanced 6 h (hour {:.2f} -> {:.2f}, days passed {:.3f})", hourBefore, hour,
                        calendar->gameDaysPassed->value);
                } else {
                    spdlog::error("Blackout: step 6: no Calendar, game time not advanced");
                }

                if (auto* spell = HangoverSpell()) {
                    if (player->HasSpell(spell)) {
                        spdlog::info("Blackout: step 6: hangover already active, not added again");
                    } else {
                        const bool ok = player->AddSpell(spell);
                        spdlog::info("Blackout: step 6: hangover ability added -> {}", ok);
                    }
                    const double now = NowHours();
                    if (now >= 0.0) {
                        g_hangoverExpiry = now + 24.0;
                        spdlog::info("Blackout: step 6: hangover expires at game hour {:.2f}", g_hangoverExpiry);
                    }
                } else {
                    spdlog::error("Blackout: step 6: hangover spell not found in {}", Forms::kPluginName);
                }

                // Hold the black screen a moment, then Tick() finishes the fade back in.
                g_phase    = Phase::kBlack;
                g_deadline = std::chrono::steady_clock::now() + kBlackTime;
                spdlog::info("Blackout: sequence done, holding black for {} ms", kBlackTime.count());
            } catch (const std::exception& e) {
                spdlog::error("Blackout: exception: {}", e.what());
                int missing = 0;
                if (chest && !moved.empty()) {
                    missing = Rollback(player, chest, moved);
                }
                Abort("unexpected error", true, missing);
            } catch (...) {
                spdlog::error("Blackout: unknown exception");
                int missing = 0;
                if (chest && !moved.empty()) {
                    missing = Rollback(player, chest, moved);
                }
                Abort("unexpected error", true, missing);
            }
        }

        void Tick()
        {
            const auto now = std::chrono::steady_clock::now();
            if (g_phase == Phase::kFadingOut && g_runGeneration != g_generation.load()) {
                Abort("the game state changed during the fade (death, load or revert); nothing was touched", false);
                return;
            }
            if (g_phase == Phase::kFadingOut && now >= g_deadline) {
                TriggerImod(kFadeToBlackHold, "FadeToBlackHold");
                DoWork();
            } else if (g_phase == Phase::kBlack && now >= g_deadline) {
                Finish(true);
            }
        }

        void PollHotkey()
        {
            static bool wasDown = false;
            const auto  key     = Config::Get().debugBlackoutKey;
            if (key == 0) {
                wasDown = false;
                return;
            }
            auto* devices  = RE::BSInputDeviceManager::GetSingleton();
            auto* keyboard = devices ? devices->GetKeyboard() : nullptr;
            if (!keyboard) {
                return;
            }
            const bool down = keyboard->IsPressed(key);
            if (down && !wasDown) {
                auto* ui = RE::UI::GetSingleton();
                if (ui && (ui->IsMenuOpen(RE::Console::MENU_NAME) || MenuOpen())) {
                    spdlog::info("Blackout: hotkey ignored, a menu is open");
                } else {
                    spdlog::info("Blackout: hotkey pressed (scan code {})", key);
                    Run("hotkey");
                }
            }
            wasDown = down;
        }

        // Bumps the generation once when the player dies.
        void WatchDeath()
        {
            auto*      player = RE::PlayerCharacter::GetSingleton();
            const bool dead   = player && player->IsDead();
            if (dead && !g_wasDead) {
                BumpGeneration("player died");
            }
            g_wasDead = dead;
        }

        void PollOnGameThread()
        {
            static int ticks = 0;
            if (++ticks % 10 == 0) {
                Config::ReloadIfChanged();
            }
            PollHotkey();
            WatchDeath();
            Tick();
            TickHangover();
            Doses::Tick();
            Withdrawal::Tick();
        }

        void PollerLoop()
        {
            for (;;) {
                std::this_thread::sleep_for(kPollInterval);
                if (g_taskPending.exchange(true)) {
                    continue;
                }
                auto* tasks = SKSE::GetTaskInterface();
                if (!tasks) {
                    g_taskPending = false;
                    continue;
                }
                tasks->AddTask([] {
                    PollOnGameThread();
                    g_taskPending = false;
                });
            }
        }
    }

    bool Busy()
    {
        return g_phase != Phase::kIdle;
    }

    bool MenuOrPauseActive()
    {
        return MenuOpen();
    }

    std::uint32_t Generation()
    {
        return g_generation.load();
    }

    void OnStateReset(const char* why)
    {
        BumpGeneration(why);
        if (g_phase.exchange(Phase::kIdle) != Phase::kIdle) {
            g_needCleanup = true;
            spdlog::warn("Blackout: a running blackout was dropped by {}", why);
        }
    }

    void OnGameLoaded()
    {
        BumpGeneration("game loaded or new game");
        g_wasDead = false;
        g_phase   = Phase::kIdle;
        if (g_needCleanup.exchange(false)) {
            StopImod(kFadeToBlack, "FadeToBlack");
            StopImod(kFadeToBlackHold, "FadeToBlackHold");
            SetControls(true);
            spdlog::info("Blackout: cleaned up a blackout interrupted by a load");
        }
    }

    void Run(const char* reason)
    {
        const char* why = reason ? reason : "?";
        spdlog::info("Blackout: Run requested (reason: {})", why);
        if (Busy()) {
            spdlog::info("Blackout: ignored, already running");
            return;
        }
        const auto settings = Config::Get();
        if (!settings.enabled) {
            spdlog::info("Blackout: ignored, Highrim is disabled");
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->GetParentCell()) {
            spdlog::info("Blackout: ignored, no player or cell");
            return;
        }
        if (player->IsDead()) {
            spdlog::info("Blackout: ignored, the player is dead");
            return;
        }
        if (player->IsInCombat()) {
            spdlog::info("Blackout: ignored, the player is in combat");
            return;
        }
        auto* map = RE::ControlMap::GetSingleton();
        if (!map || !map->AreControlsEnabled(RE::UserEvents::USER_EVENT_FLAG::kMovement)) {
            spdlog::info("Blackout: ignored, the player is not controllable");
            return;
        }
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
            spdlog::info("Blackout: ignored, in dialogue");
            return;
        }
        if (MenuOpen()) {
            spdlog::info("Blackout: ignored, the game is paused or a menu is open");
            return;
        }

        spdlog::info("Blackout: step 1: fade to black, controls off");
        Doses::CancelComedowns();  // a pending comedown cast must not fire in the middle of the blackout
        g_runGeneration = g_generation.load();
        g_reason = why;
        TriggerImod(kFadeToBlack, "FadeToBlack");
        SetControls(false);
        g_phase    = Phase::kFadingOut;
        g_deadline = std::chrono::steady_clock::now() + kFadeOutTime;
    }

    double GetHangoverExpiryHours()
    {
        return g_hangoverExpiry;
    }

    void SetHangoverExpiryHours(double hours)
    {
        g_hangoverExpiry = std::isfinite(hours) && hours > 0.0 ? hours : 0.0;
    }

    void TickHangover()
    {
        if (g_hangoverExpiry <= 0.0) {
            return;
        }
        const double now = NowHours();
        if (now < 0.0 || now < g_hangoverExpiry) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* spell  = HangoverSpell();
        if (!player || !spell) {
            return;
        }
        if (player->HasSpell(spell)) {
            const bool ok = player->RemoveSpell(spell);
            spdlog::info("Blackout: hangover expired at game hour {:.2f}, ability removed -> {}", now, ok);
        } else {
            spdlog::info("Blackout: hangover expired at game hour {:.2f}, ability was not on the player", now);
        }
        g_hangoverExpiry = 0.0;
    }

    void Install()
    {
        if (g_started.exchange(true)) {
            return;
        }
        std::thread(PollerLoop).detach();
        spdlog::info("Blackout poller started");
    }
}
