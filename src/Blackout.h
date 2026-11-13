#pragma once

#include <cstdint>

namespace Highrim::Blackout
{
    // True from Run() until the fade back in has finished.
    [[nodiscard]] bool Busy();

    // Starts the blackout sequence (see Blackout.cpp). Game thread only. Ignored when busy, disabled, dead,
    // in combat, in a menu or when the player is not controllable.
    void Run(const char* reason);

    // True while the game is paused or any menu (inventory, container, favorites, magic, map, journal, barter,
    // dialogue, console, loading, message box, ...) is open.
    [[nodiscard]] bool MenuOrPauseActive();

    // Generation counter, bumped on every game load, new game, revert and on player death. A running blackout
    // aborts when it changed since it started.
    [[nodiscard]] std::uint32_t Generation();

    // From the co-save load/revert callbacks (any thread): bump the generation, drop a running blackout's phase.
    void OnStateReset(const char* why);

    // Game thread, after a load or new game: bump the generation and clean up (fade, controls) after a blackout
    // that was running when the load happened.
    void OnGameLoaded();

    // The game time, in game hours (gameDaysPassed * 24), at which the hangover ability is removed again;
    // 0 = no hangover pending. Task 5 persists this in the save through the setter.
    [[nodiscard]] double GetHangoverExpiryHours();
    void                 SetHangoverExpiryHours(double hours);

    // Removes the hangover ability once the game time passes the expiry. Called from the poller.
    void TickHangover();

    // Starts the game-thread poller (100 ms; hotkey, blackout state machine, hangover expiry). Call once, on data loaded.
    void Install();
}
