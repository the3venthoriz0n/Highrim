#pragma once

#include <cstdint>
#include <filesystem>

// Settings live in MCM Helper's files, so they can be edited in the SkyUI Mod Configuration Menu:
//   defaults  Data\MCM\Config\Highrim\settings.ini   (shipped with the mod)
//   user      Data\MCM\Settings\Highrim.ini          (written by the MCM; overrides the defaults)
// Without MCM Helper the same files can be edited by hand. Changes are picked up within a second.
namespace Highrim::Config
{
    struct Settings
    {
        bool          enabled{ true };
        bool          overdose{ true };   // enough doses of any drug inside the window cause a blackout
        bool          addiction{ true };  // doses build dependency and withdrawal
        std::uint32_t doseWindowMinutes{ 120 };  // game minutes (5 to 240) that count as back to back
        std::uint32_t blackoutDoses{ 4 };       // 2 to 8: doses of any drug inside the window that black you out
        bool          ragdoll{ true };    // Velvet Haze knocks the player over at random
        bool          debug{ false };
        std::uint32_t debugBlackoutKey{ 0 };  // DirectX scan code; 0 = none
    };

    // Tests only: use this folder as <Data> instead of deriving it from the DLL location.
    void SetDataDirectoryForTests(std::filesystem::path a_dataDirectory);

    void Load();

    // Re-reads the files if either changed on disk. Cheap enough to call about once a second.
    void ReloadIfChanged();

    // A copy of the current settings (thread-safe).
    [[nodiscard]] Settings Get();
}

