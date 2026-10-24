#pragma once

// Temporary, developer-only switch JFG_VI_AA: picks how Original video output
// stands in for VI anti-aliasing, read once at renderer start-up, so the
// approaches can be compared without a rebuild. It is not a launcher setting
// and goes away once one approach is chosen.
//   unset, empty or "msaa": RT64 renders with MSAA (the default)
//   "off": no anti-aliasing
#include "jfg/runtime/log.hpp"
#include "n64_video/video_options.hpp"
#include <cstdlib>
#include <ostream>
#include <string>
#include <string_view>

namespace jfg {

inline constexpr const char* vi_anti_aliasing_variable = "JFG_VI_AA";

// Maps the switch's value onto the video core's option. An unknown value logs
// a warning and falls back to the default, so a typo never stops the game.
inline n64_video::AntiAliasing parse_vi_anti_aliasing_switch(
    std::string_view value, std::ostream& log) {
    if (value.empty() || value == "msaa")
        return n64_video::AntiAliasing::msaa;
    if (value == "off")
        return n64_video::AntiAliasing::off;
    write_log(log, LogLevel::warning,
        std::string(vi_anti_aliasing_variable) + "='" + std::string(value) +
            "' is not off or msaa; using msaa");
    return n64_video::AntiAliasing::msaa;
}

inline std::string vi_anti_aliasing_switch_value() {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t value_size = 0U;
    std::string result;
    if (_dupenv_s(&value, &value_size, vi_anti_aliasing_variable) == 0 && value != nullptr)
        result = value;
    std::free(value);
    return result;
#else
    const char* value = std::getenv(vi_anti_aliasing_variable);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

// Reads JFG_VI_AA from the environment.
inline n64_video::AntiAliasing read_vi_anti_aliasing_switch(std::ostream& log) {
    return parse_vi_anti_aliasing_switch(vi_anti_aliasing_switch_value(), log);
}

} // namespace jfg
