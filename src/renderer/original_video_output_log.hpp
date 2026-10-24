#pragma once

// Project glue: the one place every "Original video output: ..." line is
// logged, from the MSAA device limit, the video options and the video core's
// coverage log sink. Kept free of RT64 headers so unit tests can include it.
#include "jfg/runtime/log.hpp"
#include "jfg/runtime/video_log.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace jfg {

inline constexpr std::string_view original_video_output_log_prefix =
    "Original video output: ";

inline void log_original_video_output(LogLevel level, const std::string& message) {
    std::ostringstream line;
    write_log(line, level, std::string(original_video_output_log_prefix) + message);
    std::cerr << line.str();
    append_video_log_line(line.str());
}

}  // namespace jfg
