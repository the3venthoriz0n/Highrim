#pragma once

#include <cstddef>
#include <cstdint>

namespace RE
{
    class SpellItem;
}

// Timed spells on the player (Task 18): the "Addicted: <drug>" and withdrawal effects and the withdrawal ravage. Their length
// is not known when the plugin is written, so the live ActiveEffects are adjusted after the cast. Game thread only.
namespace Highrim::Timed
{
    // The Highrim spell with this local form id, or nullptr.
    RE::SpellItem* Lookup(std::uint32_t localId);

    // How many effects of `spell` are live on the player (not dispelled).
    int CountLive(RE::SpellItem* spell);

    // Casts the spell on the player with the instant caster. False when it could not be cast.
    bool Cast(RE::SpellItem* spell);

    // Dispels every live effect of the spell. Returns how many were flagged.
    int Dispel(RE::SpellItem* spell);

    // Sets the duration of every live effect of the spell (the hidden ones too: each effect has its own timer) to
    // elapsed + remainingSeconds. Returns how many effects were set (0 = none live yet).
    int SetRemaining(RE::SpellItem* spell, double remainingSeconds);

    // Smallest (duration - elapsed) over the live effects, in seconds; -1 when none is live.
    double RemainingSeconds(RE::SpellItem* spell);

    // Writes `magnitudes` (one per effect, in the spell's order) into the spell's effect list, so the next Cast uses them.
    // Count must match the spell's effect count. The record is rebuilt from the plugin at every game start.
    bool SetEffectMagnitudes(RE::SpellItem* spell, const float* magnitudes, std::size_t count);
}
