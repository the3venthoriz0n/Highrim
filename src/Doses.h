#pragma once

namespace Highrim::Doses
{
    // Registers the TESEquipEvent sink. Call once, on data loaded.
    void Install();

    // Game thread, from the 100 ms poller: fires a pending overdose blackout and due comedowns.
    void Tick();

    // Drops pending comedowns and any pending overdose (save load, new game, revert).
    void CancelComedowns();
}
