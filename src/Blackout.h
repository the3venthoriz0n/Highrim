#pragma once

namespace Highrim::Blackout
{
    // True from Run() until the fade back in has finished.
    [[nodiscard]] bool Busy();

    // Starts the blackout sequence (see Blackout.cpp). Game thread only. Ignored when busy, disabled, dead,
    // in combat, in a menu or when the player is not controllable.
    void Run(const char* reason);

    // The game time, in game hours (gameDaysPassed * 24), at which the hangover ability is removed again;
    // 0 = no hangover pending. Task 5 persists this in the save through the setter.
    [[nodiscard]] double GetHangoverExpiryHours();
    void                 SetHangoverExpiryHours(double hours);

    // Removes the hangover ability once the game time passes the expiry. Called from the poller.
    void TickHangover();

    // Starts the game-thread poller (100 ms; hotkey, blackout state machine, hangover expiry). Call once, on data loaded.
    void Install();
}
