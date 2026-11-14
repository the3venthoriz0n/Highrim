#include "PCH.h"

#include "Ragdoll.h"

#include "Blackout.h"
#include "Config.h"
#include "Core.h"

#include <random>
#include <string>

namespace Highrim::Ragdoll
{
    namespace
    {
        // KnockExplosion magnitude: a stumble, not a launch. Unverified in game; adjust here.
        constexpr float kKnockMagnitude = 1.0f;
        // The push comes from a point this far behind the player (game units), so it shoves them forward.
        constexpr float kKnockSourceDistance = 100.0f;

        std::mutex        g_mutex;
        Core::RagdollState g_state{};
        std::string       g_lastReason;  // last postponement reason logged (game thread)

        double RealSeconds()
        {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        // Empty when a knockdown may happen now; otherwise the first guard that failed. Only technical-safety
        // guards live here (the owner wants it to fire in combat, on a mount, swimming, in furniture and so on).
        std::string GuardFailure(RE::PlayerCharacter* player)
        {
            if (!player || !player->Is3DLoaded() || !player->GetParentCell()) return "player not loaded";
            if (player->IsDead()) return "player dead";
            // Not Actor::IsInBleedout(): CommonLibSSE-NG defines it with RELOCATION_ID(48461, 0), which is a null call on
            // Skyrim AE and 1.7.x (it crashed the game). The life state is plain data.
            if (const auto* state = player->AsActorState(); state && state->IsBleedingOut()) return "bleedout";
            if (Blackout::MenuOrPauseActive()) return "menu open or paused";
            auto* map = RE::ControlMap::GetSingleton();
            if (!map || !map->AreControlsEnabled(RE::UserEvents::USER_EVENT_FLAG::kMovement)) return "controls disabled";
            if (Blackout::Busy()) return "blackout running";
            if (player->IsInRagdollState()) return "already ragdolled";
            if (player->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) return "knock state not normal";
            return {};
        }

        // Returns the route that ran, or nullptr if neither worked.
        const char* Knock(RE::PlayerCharacter* player)
        {
            if (auto* process = player->GetActorRuntimeData().currentProcess) {
                const float angle = player->GetAngleZ();
                auto        pos   = player->GetPosition();
                pos.x -= std::sin(angle) * kKnockSourceDistance;
                pos.y -= std::cos(angle) * kKnockSourceDistance;
                process->KnockExplosion(player, pos, kKnockMagnitude);
                return "KnockExplosion";
            }
            if (player->NotifyAnimationGraph("ragdollInstant")) return "ragdollInstant";
            return nullptr;
        }
    }

    void OnDepressantDose()
    {
        std::lock_guard lock(g_mutex);
        Core::OnRagdollDose(g_state, RealSeconds());
    }

    void Reset()
    {
        std::lock_guard lock(g_mutex);
        g_state = {};
    }

    const char* KnockNow(const char* reason)
    {
        const char* why = reason ? reason : "?";
        try {
            auto*      player  = RE::PlayerCharacter::GetSingleton();
            const auto failure = GuardFailure(player);
            if (!failure.empty()) {
                spdlog::info("Ragdoll: knock for {} skipped ({})", why, failure);
                return nullptr;
            }
            const char* route = Knock(player);
            if (route) {
                spdlog::info("Ragdoll: knocked down for {} (route {})", why, route);
            } else {
                spdlog::warn("Ragdoll: knock for {} failed (no process and the animation event was refused)", why);
            }
            return route;
        } catch (const std::exception& e) {
            spdlog::error("Ragdoll: exception in KnockNow: {}", e.what());
        } catch (...) {
            spdlog::error("Ragdoll: unknown exception in KnockNow");
        }
        return nullptr;
    }

    void Tick(bool highActive, int stack)
    {
        try {
            const auto settings = Config::Get();
            const bool active   = highActive && settings.enabled && settings.ragdoll;
            static std::mt19937 rng{ std::random_device{}() };
            const double        r   = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
            const double        now = RealSeconds();

            if (!active) {
                std::lock_guard lock(g_mutex);
                Core::TickRagdoll(g_state, now, false, stack, r);
                g_lastReason.clear();
                return;
            }
            auto*       player = RE::PlayerCharacter::GetSingleton();
            const auto  reason = GuardFailure(player);
            if (!reason.empty()) {
                // Postponed: the schedule is not advanced, a due knockdown happens once the guards clear.
                if (reason != g_lastReason) {
                    spdlog::info("Ragdoll: postponed ({})", reason);
                    g_lastReason = reason;
                }
                return;
            }
            g_lastReason.clear();
            bool cast = false;
            double nextIn = 0.0;
            {
                std::lock_guard lock(g_mutex);
                cast = Core::TickRagdoll(g_state, now, true, stack, r);
                if (g_state.nextDue) nextIn = *g_state.nextDue - now;
            }
            if (!cast) return;
            const char* route = Knock(player);
            if (route) {
                spdlog::info("Ragdoll: knocked down (stack {}, next in {:.0f} s, route {})", stack, nextIn, route);
            } else {
                spdlog::warn("Ragdoll: knockdown failed (stack {}, no process and the animation event was refused)", stack);
            }
        } catch (const std::exception& e) {
            spdlog::error("Ragdoll: exception in tick: {}", e.what());
        } catch (...) {
            spdlog::error("Ragdoll: unknown exception in tick");
        }
    }
}
