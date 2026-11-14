#pragma once

namespace Highrim::Ragdoll
{
    // From a Velvet Haze dose (any thread): restarts the knockdown schedule and the post-dose quiet time.
    void OnDepressantDose();

    // Save load, new game, revert, blackout start (any thread): forgets the schedule.
    void Reset();

    // Game thread, from the 100 ms poller. `highActive` = a Velvet Haze high is running (dose until its comedown),
    // `stack` = the depressant stack. Applies the guards, then knocks the player over when one is due.
    void Tick(bool highActive, int stack);

    // Game thread. Knocks the player down right now for `reason` (a blackout is about to start). Applies only the
    // technical-safety guards: ignores the schedule, the bRagdoll setting and the 6 s minimum gap. Returns the route
    // that ran ("KnockExplosion" or "ragdollInstant"), or nullptr when a guard blocked it or no route worked (the
    // cause is logged).
    const char* KnockNow(const char* reason);
}
