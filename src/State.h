#pragma once

#include "Core.h"

namespace Highrim::State
{
    inline constexpr std::uint32_t kRecordId      = 'HRIM';
    inline constexpr std::uint32_t kRecordVersion = 1;

    struct DoseResult
    {
        bool            overdose{ false };
        Core::Addiction after{};
    };

    // Registers the SKSE co-save callbacks (unique id 'HRIM'). Call once from SKSEPluginLoad.
    void Register();

    // Records one dose at the given game hour. Overdose is judged against the most recent dose of any type
    // before this one. When addiction is off the levels do not change but the dose time is still tracked.
    DoseResult RecordDose(Core::DrugType type, double nowHours, int windowMinutes, bool addictionOn);

    [[nodiscard]] Core::Addiction Snapshot();

    // Called after a save has finished loading (or a new game started): re-applies the hangover ability if its
    // expiry is still in the future and cancels pending comedowns.
    void OnGameLoaded();
}
