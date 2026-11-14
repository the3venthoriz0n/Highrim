#include "Config.h"

#include <Windows.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace Highrim::Config
{
    namespace
    {
        using FileTime = std::optional<std::filesystem::file_time_type>;

        std::mutex g_mutex;
        Settings   g_settings{};
        FileTime   g_defaultsTime;
        FileTime   g_userTime;
        std::optional<std::filesystem::path> g_dataOverride;

        std::filesystem::path DataDirectory()
        {
            if (g_dataOverride) {
                return *g_dataOverride;
            }
            // The DLL is in <Data>\SKSE\Plugins; derive <Data> from its location.
            HMODULE module = nullptr;
            GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&DataDirectory),
                &module);

            char buffer[MAX_PATH]{};
            if (module && GetModuleFileNameA(module, buffer, MAX_PATH) != 0) {
                return std::filesystem::path(buffer).parent_path().parent_path().parent_path();
            }
            return std::filesystem::path("Data");
        }

        std::filesystem::path DefaultsPath()
        {
            return DataDirectory() / "MCM" / "Config" / "Highrim" / "settings.ini";
        }

        std::filesystem::path UserPath()
        {
            return DataDirectory() / "MCM" / "Settings" / "Highrim.ini";
        }

        FileTime StampOf(const std::filesystem::path& a_path)
        {
            std::error_code ec;
            const auto      time = std::filesystem::last_write_time(a_path, ec);
            return ec ? FileTime{} : FileTime{ time };
        }

        // GetPrivateProfile* resolves relative paths against the Windows directory, so always pass
        // absolute paths. Returns nothing if the key is not present in the file.
        std::optional<std::string> ReadText(const std::filesystem::path& a_path, const char* a_section, const char* a_key)
        {
            char text[128]{};
            GetPrivateProfileStringA(a_section, a_key, "\x01", text, sizeof(text), a_path.string().c_str());
            if (text[0] == '\x01') {
                return std::nullopt;
            }
            return std::string(text);
        }

        // The user's value wins over the shipped default, which wins over the built-in default.
        std::string Text(const char* a_section, const char* a_key, const std::string& a_builtIn)
        {
            if (auto user = ReadText(UserPath(), a_section, a_key)) {
                return *user;
            }
            if (auto shipped = ReadText(DefaultsPath(), a_section, a_key)) {
                return *shipped;
            }
            return a_builtIn;
        }

        bool Flag(const char* a_section, const char* a_key, bool a_builtIn)
        {
            const auto text = Text(a_section, a_key, a_builtIn ? "1" : "0");
            return !text.empty() && text != "0";
        }

        std::uint32_t Number(const char* a_section, const char* a_key, std::uint32_t a_builtIn, std::uint32_t a_min, std::uint32_t a_max)
        {
            const auto text = Text(a_section, a_key, std::to_string(a_builtIn));
            try {
                const long value = std::stol(text);
                return static_cast<std::uint32_t>(std::clamp<long>(value, a_min, a_max));
            } catch (...) {
                return a_builtIn;
            }
        }

        Settings Read()
        {
            Settings s;
            s.enabled = Flag("General", "bEnabled", s.enabled);
            s.overdose = Flag("General", "bOverdose", s.overdose);
            s.addiction = Flag("General", "bAddiction", s.addiction);
            s.doseWindowMinutes = Number("General", "iDoseWindowMinutes", s.doseWindowMinutes, 5, 240);
            s.blackoutDoses = Number("General", "iBlackoutDoses", s.blackoutDoses, 2, 8);
            s.ragdoll = Flag("General", "bRagdoll", s.ragdoll);
            s.rimerushWithdrawalBlackout = Flag("General", "bRimerushWithdrawalBlackout", s.rimerushWithdrawalBlackout);
            s.fullBlackout = Flag("General", "bFullBlackout", s.fullBlackout);
            s.debug = Flag("General", "bDebug", s.debug);
            s.debugBlackoutKey = Number("General", "iDebugBlackoutKey", s.debugBlackoutKey, 0, 1000);
            return s;
        }
    }

    void SetDataDirectoryForTests(std::filesystem::path a_dataDirectory)
    {
        std::scoped_lock lock(g_mutex);
        g_dataOverride = std::move(a_dataDirectory);
    }

    void Load()
    {
        std::scoped_lock lock(g_mutex);
        g_settings = Read();
        g_defaultsTime = StampOf(DefaultsPath());
        g_userTime = StampOf(UserPath());
    }

    void ReloadIfChanged()
    {
        const auto defaults = StampOf(DefaultsPath());
        const auto user = StampOf(UserPath());

        std::scoped_lock lock(g_mutex);
        if (defaults == g_defaultsTime && user == g_userTime) {
            return;
        }
        g_settings = Read();
        g_defaultsTime = defaults;
        g_userTime = user;
    }

    Settings Get()
    {
        std::scoped_lock lock(g_mutex);
        return g_settings;
    }
}


