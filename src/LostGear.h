#pragma once

namespace Highrim::LostGear
{
    // "Lost Gear: <Hold> Prison": a lasting ability in Active Effects (one per hold) that says where the player's
    // things are after a blackout. It ends when the player enters the prison's cell or the confiscation chest's cell,
    // or after Core::kLostGearHours game hours. Game thread only, except the co-save accessors.

    // After a blackout moved the player's things to the prison of hold `holdIndex` (index into Prison::Holds()).
    void Begin(int holdIndex);

    // Co-save: the pending hold (-1 = none) and the game hour the reminder ends. Set on load, before OnGameLoaded.
    [[nodiscard]] int    GetHold();
    [[nodiscard]] double GetExpiryHours();
    void                 Set(int holdIndex, double expiryHours);

    // Save loaded or new game: re-applies the ability for the pending hold (or removes stray ones).
    void OnGameLoaded();

    // From the 100 ms poller: ends the reminder on expiry or when the player is in the prison / chest cell.
    void Tick();
}
