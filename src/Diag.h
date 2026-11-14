#pragma once

#include <cstdint>
#include <string>

namespace Highrim::Diag
{
    // Game thread. After `delaySeconds` of real time, logs `Highrim <label> active: N of M` for the spell with this
    // local id in Highrim.esp: M effects in the spell, N of them active on the player right now. Logs an error when
    // N < M. This is the same kind of check the dose diagnostics do for the potions (it proves what the engine
    // reports, nothing about what the screen shows).
    void Schedule(std::uint32_t localSpellId, const std::string& label, double delaySeconds = 0.5);

    // From the 100 ms poller.
    void Tick();

    // Save loaded or new game: forget pending checks.
    void Clear();
}
