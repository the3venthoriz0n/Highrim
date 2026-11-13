#include "PCH.h"

#include "State.h"

#include "Blackout.h"
#include "Doses.h"
#include "Forms.h"
#include "Withdrawal.h"

namespace Highrim::State
{
    namespace
    {
        std::mutex      g_mutex;
        Core::Addiction g_addiction{};

        void Reset()
        {
            {
                std::lock_guard lock(g_mutex);
                g_addiction = Core::Addiction{};
            }
            Blackout::SetHangoverExpiryHours(0.0);
            Blackout::OnStateReset("revert or load");
        }

        void OnRevert(SKSE::SerializationInterface*)
        {
            Reset();
            Doses::CancelComedowns();
            spdlog::info("State: revert, addiction and hangover reset");
        }

        void OnSave(SKSE::SerializationInterface* intfc)
        {
            Core::PersistedState p{};
            {
                std::lock_guard lock(g_mutex);
                p.addiction = g_addiction;
            }
            p.hangoverExpiryHours = Blackout::GetHangoverExpiryHours();
            const std::string text = Core::SerializeState(p);
            if (!intfc->WriteRecord(kRecordId, kRecordVersion, text.data(), static_cast<std::uint32_t>(text.size()))) {
                spdlog::error("State: failed to write the co-save record");
                return;
            }
            spdlog::info("State: saved ({} bytes), hangover expiry {:.2f}", text.size(), p.hangoverExpiryHours);
        }

        void OnLoad(SKSE::SerializationInterface* intfc)
        {
            Reset();
            Doses::CancelComedowns();
            std::uint32_t type = 0, version = 0, length = 0;
            bool          found = false;
            while (intfc->GetNextRecordInfo(type, version, length)) {
                if (type != kRecordId) {
                    continue;
                }
                found = true;
                if (version != kRecordVersion || length == 0 || length > 4096) {
                    spdlog::error("State: unusable record (version {}, length {}), reset to zero", version, length);
                    continue;
                }
                std::string text(length, '\0');
                if (intfc->ReadRecordData(text.data(), length) != length) {
                    spdlog::error("State: short read, reset to zero");
                    continue;
                }
                auto parsed = Core::DeserializeState(text);
                if (!parsed) {
                    spdlog::error("State: corrupt record, reset to zero");
                    continue;
                }
                {
                    std::lock_guard lock(g_mutex);
                    g_addiction = parsed->addiction;
                }
                Blackout::SetHangoverExpiryHours(parsed->hangoverExpiryHours);
                spdlog::info("State: loaded, levels {:.1f}/{:.1f}/{:.1f}, hangover expiry {:.2f}",
                    parsed->addiction.level[0], parsed->addiction.level[1], parsed->addiction.level[2],
                    parsed->hangoverExpiryHours);
            }
            if (!found) {
                spdlog::info("State: no record in this save, starting at zero");
            }
        }
    }

    void Register()
    {
        auto* ser = SKSE::GetSerializationInterface();
        if (!ser) {
            spdlog::critical("State: no serialization interface");
            return;
        }
        ser->SetUniqueID(kRecordId);
        ser->SetSaveCallback(OnSave);
        ser->SetLoadCallback(OnLoad);
        ser->SetRevertCallback(OnRevert);
    }

    DoseResult RecordDose(Core::DrugType type, double nowHours, bool addictionOn)
    {
        std::lock_guard lock(g_mutex);
        DoseResult      r;
        const Core::Addiction before = g_addiction;
        Core::AddDose(g_addiction, type, nowHours);
        if (!addictionOn) {
            for (int i = 0; i < 3; ++i) {
                g_addiction.level[i] = before.level[i];
            }
            g_addiction.lastUpdateHours = before.lastUpdateHours;
        }
        r.after = g_addiction;
        return r;
    }

    Core::Addiction Snapshot()
    {
        std::lock_guard lock(g_mutex);
        return g_addiction;
    }

    void OnGameLoaded()
    {
        Blackout::OnGameLoaded();
        Withdrawal::OnGameLoaded();
        Doses::CancelComedowns();
        double expiry   = Blackout::GetHangoverExpiryHours();
        auto*  calendar = RE::Calendar::GetSingleton();
        auto*  player   = RE::PlayerCharacter::GetSingleton();
        auto*  handler  = RE::TESDataHandler::GetSingleton();
        if (!calendar || !calendar->gameDaysPassed || !player || !handler) {
            if (expiry > 0.0) {
                spdlog::warn("State: cannot restore the hangover, game objects missing");
            }
            return;
        }
        const double now = static_cast<double>(calendar->gameDaysPassed->value) * 24.0;
        if (expiry <= 0.0) {
            // No expiry recorded (missing or corrupt co-save). If the player still carries the ability, it would
            // never be removed: give it a fresh 24 game hours.
            auto* carried = handler->LookupForm<RE::SpellItem>(Forms::HighrimHangover, Forms::kPluginName);
            if (carried && player->HasSpell(carried)) {
                expiry = now + 24.0;
                Blackout::SetHangoverExpiryHours(expiry);
                spdlog::warn("State: hangover ability present but no expiry recorded, set to game hour {:.2f} (now + 24 h)", expiry);
            }
            return;
        }
        if (now >= expiry) {
            return;  // TickHangover removes it
        }
        auto* spell = handler->LookupForm<RE::SpellItem>(Forms::HighrimHangover, Forms::kPluginName);
        if (!spell) {
            spdlog::error("State: hangover spell not found in {}", Forms::kPluginName);
            return;
        }
        if (player->HasSpell(spell)) {
            spdlog::info("State: hangover still active until game hour {:.2f}, already on the player", expiry);
        } else {
            const bool ok = player->AddSpell(spell);
            spdlog::info("State: hangover restored until game hour {:.2f}, AddSpell -> {}", expiry, ok);
        }
    }
}
