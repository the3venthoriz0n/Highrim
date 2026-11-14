#include "PCH.h"

#include "Config.h"
#include "Blackout.h"
#include "Doses.h"
#include "State.h"
#include "Prison.h"

namespace
{
    void SetupLog()
    {
        auto logDirectory = SKSE::log::log_directory();
        if (!logDirectory) {
            return;
        }

        auto logPath = *logDirectory / "Highrim.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
        auto logger = std::make_shared<spdlog::logger>("Highrim", std::move(sink));

        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [Highrim] [%l] %v");
        spdlog::set_default_logger(std::move(logger));
    }

    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (a_message && (a_message->type == SKSE::MessagingInterface::kPostLoadGame ||
                          a_message->type == SKSE::MessagingInterface::kNewGame)) {
            Highrim::State::OnGameLoaded();
        }
        if (a_message && a_message->type == SKSE::MessagingInterface::kDataLoaded) {
            spdlog::info("Data loaded");
            Highrim::Config::Load();
            const auto settings = Highrim::Config::Get();
            if (settings.debug) {
                spdlog::set_level(spdlog::level::debug);
                spdlog::default_logger()->set_level(spdlog::level::debug);
            }
            Highrim::Prison::Discover();
            Highrim::Blackout::Install();
            Highrim::Doses::Install();
            spdlog::info("Settings: enabled={} overdose={} addiction={} doseWindowMinutes={} blackoutDoses={} debug={} debugBlackoutKey={}",
                settings.enabled, settings.overdose, settings.addiction, settings.doseWindowMinutes,
                settings.blackoutDoses, settings.debug, settings.debugBlackoutKey);
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SetupLog();
    spdlog::info("Highrim 0.2.0 loading");

    Highrim::Config::Load();
    if (Highrim::Config::Get().debug) {
        // The logger itself was created at info level; raise its verbosity as well as the global one.
        spdlog::set_level(spdlog::level::debug);
        spdlog::default_logger()->set_level(spdlog::level::debug);
    }

    Highrim::State::Register();

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
        spdlog::critical("Could not register SKSE messaging listener");
        return false;
    }

    return true;
}
