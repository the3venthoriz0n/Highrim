#pragma once

namespace Highrim::Withdrawal
{
    // Game thread, from the 100 ms poller: every ~5 real seconds applies the withdrawal ability for the current
    // tier (never more than one) and casts the blur pulse. Does nothing in menus/loading screens or with no player.
    void Tick();

    // Save loaded or new game: forget what is applied so the next tick re-applies exactly once.
    void OnGameLoaded();
}
