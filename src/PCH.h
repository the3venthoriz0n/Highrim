#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

// CommonLibSSE-NG generates __HighrimPlugin.cpp with "..."sv.
using namespace std::string_view_literals;

// <Windows.h> defines GetObject as GetObjectA/W, which breaks RE:: members of the same name.
#ifdef GetObject
#  undef GetObject
#endif
