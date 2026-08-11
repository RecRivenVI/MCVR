#pragma once

#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <syncstream>
#include <string_view>
#include <thread>

namespace mcvr::log {

enum class Level { Debug, Info, Warn, Error };

inline std::string_view levelName(Level level) noexcept {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

/** Writes the same prefix shape used by Minecraft's Log4j console layout. */
inline std::osyncstream stream(Level level, std::string_view component, std::ostream &target) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &nowTime);
#else
    localtime_r(&nowTime, &local);
#endif
    const auto thread = static_cast<unsigned long long>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffu);
    std::osyncstream output(target);
    output << '[' << std::put_time(&local, "%H:%M:%S") << "] [Native-"
           << std::hex << thread << std::dec << '/' << levelName(level) << "] [MCVR/"
           << component << "]: ";
    return output;
}

inline std::osyncstream debug(std::string_view component) {
    return stream(Level::Debug, component, std::cout);
}

inline std::osyncstream info(std::string_view component) {
    return stream(Level::Info, component, std::cout);
}

inline std::osyncstream warn(std::string_view component) {
    return stream(Level::Warn, component, std::cerr);
}

inline std::osyncstream error(std::string_view component) {
    return stream(Level::Error, component, std::cerr);
}

} // namespace mcvr::log
