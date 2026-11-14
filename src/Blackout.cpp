#include "PCH.h"

#include "Blackout.h"

#include "Config.h"
#include "Core.h"
#include "Diag.h"
#include "Doses.h"
#include "Forms.h"
#include "LostGear.h"
#include "Prison.h"
#include "Ragdoll.h"
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

        // Vanilla image space modifiers (Skyrim.esm, confirmed by parsing it).
        constexpr RE::FormID kFadeToBlack     = 0x000F756D;
        constexpr RE::FormID kFadeToBlackHold = 0x000F756E;
        constexpr RE::FormID kFadeToBlackBack = 0x000F756F;
        
        std::atomic_bool g_started{ false };
        std::atomic_bool g_taskPending{ false };

        // All of these are touched on the game thread only (the expiry is also read by Task 5's save callbacks,
        // which run on the game thread too).
        double g_hangoverExpiry = 0.0;

        enum class Phase { kIdle, kFadingOut, kSettling, kBlack };
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

        // What the settle check after the teleport needs (game thread only). The items are already in the chest at this
        // point, so a move that never happens puts them back (Rollback) before the blackout is given up.
        struct SettleContext
        {
            std::vector<Moved> moved;
            RE::FormID         chest     = 0;
            RE::FormID         spot      = 0;
            std::size_t        holdIndex = 0;
            std::string        holdName;
            std::uint32_t      noteLocalId = 0;
            Core::Point3       before{};
            Core::Point3       target{};
            int                method = 1;  // 1 = MoveTo, 2 = console moveto
        };
        SettleContext g_settle;
        std::string   g_wakeMessage;

        Core::Point3 PlayerPoint(RE::PlayerCharacter* player)
        {
            const auto p = player->GetPosition();
            return { p.x, p.y, p.z };
        }

        // Method 2: the console command `player.moveto <marker>`, which handles a target in another cell or worldspace.
        bool ConsoleMoveTo(RE::PlayerCharacter* player, RE::FormID target)
        {
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
            auto* script  = factory ? factory->Create() : nullptr;
            if (!script) {
                spdlog::error("Blackout: step 6: cannot create a console script");
                return false;
            }
            script->SetCommand(fmt::format("player.moveto {:08X}", target));
            script->CompileAndRun(player);
            delete script;
            return true;
        }

        void Finish(bool success, int missing = 0)
        {
            StopImod(kFadeToBlackHold, "FadeToBlackHold");
            TriggerImod(kFadeToBlackBack, "FadeToBlackBack");
            SetControls(true);
            if (success) {
                auto* p = RE::PlayerCharacter::GetSingleton();
                if (p && !p->IsDead()) {
                    RE::SendHUDMessage::ShowHUDMessage(g_wakeMessage.empty() ? "You wake up with a splitting headache." : g_wakeMessage.c_str());
                    RE::SendHUDMessage::ShowHUDMessage(
                        "Hangover: stamina is 30 points lower and regenerates 25% slower, and your vision blurs now and then, for one game day.",
                        nullptr, false);
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

        // Step 6b, once the player is known to be at the wake spot: note, game time, hangover, Lost Gear reminder.
        void FinishStep6(RE::PlayerCharacter* player)
        {
            auto* handler = RE::TESDataHandler::GetSingleton();
            // LookupForm<T> requires T::FORMTYPE to equal the form type; TESBoundObject has none, so asking for it always
            // failed ("note not found"). The note is a BOOK, so ask for TESObjectBOOK.
            spdlog::info("Blackout: step 6: looking up the {} note: local id {:06X} in {}", g_settle.holdName, g_settle.noteLocalId, Forms::kPluginName);
            auto* note = handler ? handler->LookupForm<RE::TESObjectBOOK>(g_settle.noteLocalId, Forms::kPluginName) : nullptr;
            if (note) {
                player->AddObjectToContainer(note, nullptr, 1, nullptr);
                spdlog::info("Blackout: step 6: note given ({} {:08X}), player now has {}; text: {}", note->GetName(), note->GetFormID(),
                    CountIn(player, note), Core::NoteText(g_settle.holdName + " prison"));
            } else {
                spdlog::error("Blackout: step 6: note for {} not found in {} (local id {:06X})", g_settle.holdName, Forms::kPluginName,
                    g_settle.noteLocalId);
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
                Diag::Schedule(Forms::HighrimHangover, "hangover");
                const double now = NowHours();
                if (now >= 0.0) {
                    g_hangoverExpiry = now + 24.0;
                    spdlog::info("Blackout: step 6: hangover expires at game hour {:.2f}", g_hangoverExpiry);
                }
            } else {
                spdlog::error("Blackout: step 6: hangover spell not found in {}", Forms::kPluginName);
            }

            LostGear::Begin(static_cast<int>(g_settle.holdIndex));
            g_wakeMessage = Core::LostGearMessage(g_settle.holdName);
            g_settle      = SettleContext{};

            // Hold the black screen a moment, then Tick() finishes the fade back in.
            g_phase    = Phase::kBlack;
            g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            spdlog::info("Blackout: sequence done, holding black a moment longer");
        }

        // Called from Tick() (kSettling) when the settle delay has passed: did the player move?
        void CheckSettle()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* chest  = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_settle.chest);
            std::string why;
            if (g_runGeneration != g_generation.load()) {
                why = "the game was loaded or reverted";
            } else if (!player) {
                why = "no player";
            } else if (player->IsDead()) {
                why = "the player died";
            } else if (!chest) {
                why = "the prison chest is gone";
            }
            if (!why.empty()) {
                spdlog::error("Blackout: step 6: cannot go on: {}, rolling back", why);
                int missing = 0;
                if (chest && player) {
                    missing = Rollback(player, chest, g_settle.moved);
                }
                g_settle = SettleContext{};
                Abort("cancelled while settling after the teleport: " + why, player && !player->IsDead(), missing);
                return;
            }
            const auto  after   = PlayerPoint(player);
            const auto  verdict = Core::CheckMove(g_settle.before, g_settle.target, after);
            const char* word    = verdict == Core::MoveVerdict::kMoved ? "moved"
                                  : verdict == Core::MoveVerdict::kNotApplicable ? "target was already within 1000 units, cannot tell"
                                                                                  : "NOT moved";
            spdlog::info("Blackout: step 6: position after settle ({:.0f},{:.0f},{:.0f}) from ({:.0f},{:.0f},{:.0f}), target ({:.0f},{:.0f},{:.0f}): {} (method {})",
                after.x, after.y, after.z, g_settle.before.x, g_settle.before.y, g_settle.before.z, g_settle.target.x, g_settle.target.y,
                g_settle.target.z, word, g_settle.method == 1 ? "MoveTo" : "console moveto");
            if (verdict != Core::MoveVerdict::kNotMoved) {
                spdlog::info("Blackout: step 6: teleport method {} worked", g_settle.method == 1 ? "MoveTo" : "console moveto");
                FinishStep6(player);
                return;
            }
            if (g_settle.method == 1) {
                g_settle.method = 2;
                const bool ran  = ConsoleMoveTo(player, g_settle.spot);
                spdlog::warn("Blackout: step 6: MoveTo did not move the player; trying the console moveto -> {}", ran);
                g_deadline = std::chrono::steady_clock::now() +
                             std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(Core::kMoveSettleSeconds));
                return;
            }
            spdlog::error("Blackout: step 6: no teleport method moved the player, rolling back");
            const int missing = Rollback(player, chest, g_settle.moved);
            g_settle          = SettleContext{};
            Abort("the teleport did not work", true, missing);
        }

        // Steps 2 to 6a of the sequence, run once the screen is black. Game thread.
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
                // Step 2: the hold whose prison is nearest the player (or whose capital the player is in).
                spdlog::info("Blackout: step 2: choosing the nearest hold prison");
                const auto place = Prison::LocatePlayer(player);
                if (place.tamriel) {
                    spdlog::info("Blackout: step 2: player in worldspace {:08X}, Tamriel position ({:.0f},{:.0f}); {}", place.world,
                        place.tamriel->x, place.tamriel->y, place.how);
                } else {
                    spdlog::warn("Blackout: step 2: player in worldspace {:08X}, no Tamriel position; {}", place.world, place.how);
                }

                std::vector<Prison::Resolved> resolved;
                for (const auto& def : Prison::Holds()) {
                    resolved.push_back(Prison::Resolve(def));
                }
                const auto holdInfos  = Prison::HoldInfos(resolved);
                const auto worldHolds = Prison::WorldHolds();
                for (std::size_t i = 0; i < resolved.size(); ++i) {
                    if (!holdInfos[i].hasChest) {
                        spdlog::info("Blackout: step 2: {} is not a candidate ({})", resolved[i].def->name,
                            resolved[i].chest ? "no Tamriel anchor in GameData.h" : "no confiscation chest");
                        continue;
                    }
                    const float d = place.tamriel ? std::hypot(holdInfos[i].anchor.x - place.tamriel->x, holdInfos[i].anchor.y - place.tamriel->y) : -1.0f;
                    spdlog::info("Blackout: step 2: candidate {} anchor ({:.0f},{:.0f}), {:.0f} units from the player", resolved[i].def->name,
                        holdInfos[i].anchor.x, holdInfos[i].anchor.y, d);
                }
                const auto choice = Core::ChooseHold(holdInfos, worldHolds, place.world, place.tamriel);
                if (!choice) {
                    Abort("no hold prison with a chest could be located");
                    return;
                }
                const auto& hold = resolved[choice->index];
                const auto  anchor = holdInfos[choice->index].anchor;
                chest            = hold.chest;
                spdlog::info("Blackout: step 2: chose {} ({}; chest {:08X} via {}; prison anchor ({:.0f},{:.0f}))", hold.def->name,
                    choice->reason == Core::HoldReason::kCapitalWorld ? "the player is in its capital worldspace" : "nearest by Tamriel position",
                    chest->GetFormID(), hold.chestSource, anchor.x, anchor.y);

                // Step 5 (done before any item moves, so a failure here costs nothing): a wake spot, from every Tamriel
                // map marker, in widening distance bands from the prison.
                spdlog::info("Blackout: step 5: picking a wake spot from all Tamriel map markers");
                const auto markers = Prison::CollectWakeMarkers();
                spdlog::info("Blackout: step 5: {} markers: table {} of {} resolved, {} read live from the persistent cell ({} not in the table), "
                             "{} skipped without a cell, {} moved from the table position",
                    markers.usable.size(), markers.tableFound, markers.tableTotal, markers.liveFound, markers.liveExtra, markers.noCell,
                    markers.moved);
                std::vector<Core::Point> spotPoints;
                for (const auto& m : markers.usable) {
                    spotPoints.push_back({ m.x, m.y });
                }
                static std::mt19937 rng{ std::random_device{}() };
                const float         roll = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
                const auto          spot = Core::PickWakeSpotStaged(spotPoints, anchor, roll);
                if (!spot) {
                    Abort("no wake spot at least 3000 units from the prison among " + std::to_string(markers.usable.size()) + " map markers");
                    return;
                }
                RE::TESObjectREFR* spotRef = markers.usable[spot->index].ref;
                spdlog::info("Blackout: step 5: stage {} ({}), {} candidate(s) in the band; wake spot marker {:08X} at ({:.0f},{:.0f}), {:.0f} units "
                             "from the prison anchor ({:.0f},{:.0f})",
                    spot->stage, spot->stage == 1 ? "15000-80000" : spot->stage == 2 ? "8000-150000" : "3000-unbounded", spot->inBand,
                    spotRef->GetFormID(), spotPoints[spot->index].x, spotPoints[spot->index].y, spot->distance, anchor.x, anchor.y);

                // Step 3 and 4: the chest, then the items.
                spdlog::info("Blackout: step 3: using chest {:08X} ({})", chest->GetFormID(), hold.def->name);
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

                // Step 6a: teleport. MoveTo is applied by the engine a moment later (the first playtest log read the position
                // straight after the call and saw no change, but the player did wake up elsewhere), so the position is
                // checked in Tick() after Core::kMoveSettleSeconds, still black. The rest of step 6 waits for that check.
                spdlog::info("Blackout: step 6: teleporting to marker {:08X} (cell {})", spotRef->GetFormID(),
                    spotRef->GetParentCell() ? "attached" : "not attached, the saved cell is used by the engine");
                g_settle             = SettleContext{};
                g_settle.moved       = moved;
                g_settle.chest       = chest->GetFormID();
                g_settle.spot        = spotRef->GetFormID();
                g_settle.holdIndex   = choice->index;
                g_settle.holdName    = hold.def->name;
                g_settle.noteLocalId = hold.def->noteLocalId;
                g_settle.before      = PlayerPoint(player);
                {
                    const auto t    = spotRef->GetPosition();
                    g_settle.target = { t.x, t.y, t.z };
                }
                player->MoveTo(spotRef);
                const auto right = PlayerPoint(player);
                spdlog::info("Blackout: step 6: position ({:.0f},{:.0f},{:.0f}) -> ({:.0f},{:.0f},{:.0f}) right after MoveTo (applied a moment later; checked after {:.1f} s)",
                    g_settle.before.x, g_settle.before.y, g_settle.before.z, right.x, right.y, right.z, Core::kMoveSettleSeconds);
                g_phase    = Phase::kSettling;
                g_deadline = std::chrono::steady_clock::now() +
                             std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(Core::kMoveSettleSeconds));
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
            } else if (g_phase == Phase::kSettling && now >= g_deadline) {
                try {
                    CheckSettle();
                } catch (...) {
                    spdlog::error("Blackout: exception while settling after the teleport");
                    Abort("unexpected error", true);
                }
            } else if (g_phase == Phase::kBlack && now >= g_deadline) {
                Finish(true);
            }
        }

        // Set when the hotkey knocked the player down; the blackout starts when it passes.
        std::optional<std::chrono::steady_clock::time_point> g_hotkeyDue;
        std::uint32_t                                         g_hotkeyGeneration = 0;

        void PollHotkeyDue()
        {
            if (!g_hotkeyDue || std::chrono::steady_clock::now() < *g_hotkeyDue) {
                return;
            }
            g_hotkeyDue.reset();
            if (g_hotkeyGeneration != g_generation.load()) {
                spdlog::info("Blackout: hotkey blackout dropped, a load, new game or death happened meanwhile");
                return;
            }
            Run("hotkey");
        }

        void PollHotkey()
        {
            PollHotkeyDue();
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
                    if (g_hotkeyDue) {
                        spdlog::info("Blackout: hotkey ignored, a blackout is already about to start");
                    } else {
                        const char* route = Ragdoll::KnockNow("hotkey blackout");
                        spdlog::info("Blackout: ragdoll first (route {}), starting in {:.1f} s", route ? route : "none",
                            Core::kRagdollFirstSeconds);
                        g_hotkeyDue = std::chrono::steady_clock::now() +
                                      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                          std::chrono::duration<double>(Core::kRagdollFirstSeconds));
                        g_hotkeyGeneration = g_generation.load();
                    }
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
            Diag::Tick();
            LostGear::Tick();
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
        g_settle  = SettleContext{};
        g_wakeMessage.clear();
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
            RE::SendHUDMessage::ShowHUDMessage("The hangover wears off.");
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
