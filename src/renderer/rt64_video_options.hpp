#pragma once

// Project glue between the RT64 shell and the video core: maps JFG's shell
// configuration onto the core's options. It lives outside the core, which
// must not know any one game's types.
#include "jfg/renderer/rt64_shell.hpp"
#include "n64_video/coverage_estimate.hpp"
#include "n64_video/log_sink.hpp"
#include "n64_video/rt64_configuration.hpp"
#include "rt64_msaa_support.hpp"

namespace jfg {

inline LogLevel video_core_log_level(n64_video::LogLevel level) {
    switch (level) {
    case n64_video::LogLevel::info:
        return LogLevel::info;
    case n64_video::LogLevel::warning:
        return LogLevel::warning;
    case n64_video::LogLevel::debug:
        return LogLevel::debug;
    }
    return LogLevel::warning;
}

// Applies JFG's video settings to RT64's user configuration. Call it before
// RT64::Application::setup, after the shell has installed its render hooks.
inline void apply_video_options(const Rt64ShellConfiguration& configuration,
                                RT64::UserConfiguration& user_configuration) {
    n64_video::VideoOptions options;
    options.original_video_output = configuration.video_output == VideoOutput::original;
    // The core reports coverage statuses and the VI pre-pass's gamma dither
    // whenever Original video output runs, with or without a coverage estimate.
    if (options.original_video_output) {
        n64_video::log_sink() = [](n64_video::LogLevel level, const char* message) {
            log_original_video_output(video_core_log_level(level), message);
        };
    }
    if (n64_video::estimates_coverage(options)) {
        log_original_video_output(LogLevel::info,
            "VI anti-aliasing and divot filter from estimated coverage; "
            "MSAA resolves take RT64's raster path");
    }
    n64_video::apply_to_rt64(options, user_configuration);
    limit_msaa_to_device_at_setup(user_configuration);
}

} // namespace jfg
