#pragma once

#include <spdlog/spdlog.h>
#include <chrono>

namespace Log {

// Lets a log line through at most once per interval. For trace lines that
// would otherwise repeat every frame while a state lasts:
//   static Log::Throttle s_log;
//   if (s_log.Allow()) spdlog::trace(...);
class Throttle {
public:
    explicit Throttle(std::chrono::milliseconds interval = std::chrono::seconds(1))
        : m_interval(interval) {}

    bool Allow()
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_hasLogged && now - m_last < m_interval)
            return false;
        m_last = now;
        m_hasLogged = true;
        return true;
    }

private:
    std::chrono::milliseconds m_interval;
    std::chrono::steady_clock::time_point m_last{};
    bool m_hasLogged = false;
};

// Initialize the rotating file logger. Call once from InitThread.
// logPath: full path to the log file (e.g. next to the DLL)
// maxSizeMb: max file size before rotation (default 5 MB)
// maxFiles: number of rotated files to keep (default 3)
void Init(const char* logPath = nullptr, size_t maxSizeMb = 5, size_t maxFiles = 3);

// Shut down spdlog (flush + drop all loggers). Call on DLL_PROCESS_DETACH.
void Shutdown();

}  // namespace Log
