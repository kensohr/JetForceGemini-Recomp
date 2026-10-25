#pragma once

// Project glue between the RT64 shell and the video core: maps JFG's shell
// configuration onto the core's options. It lives outside the core, which
// must not know any one game's types.
#include "jfg/renderer/rt64_shell.hpp"
#include "crt_filter_options.hpp"
#include "n64_video/coverage_estimate.hpp"
#include "n64_video/crt_filter.hpp"
#include "n64_video/log_sink.hpp"
#include "n64_video/rt64_configuration.hpp"
#include "rt64_msaa_support.hpp"

#include <string>

namespace jfg {

inline jfg::LogLevel video_core_log_level(n64_video::LogLevel level) {
    switch (level) {
    case n64_video::LogLevel::debug:
        return jfg::LogLevel::debug;
    case n64_video::LogLevel::info:
        return jfg::LogLevel::info;
    case n64_video::LogLevel::warning:
        return jfg::LogLevel::warning;
    }
    return jfg::LogLevel::warning;
}

// Reports which CRT preset Original video output draws through, and whether
// it bends the picture (screen curvature). Under Enhanced the stored preset
// is ignored, so nothing is reported.
inline void log_crt_filter(const n64_video::VideoOptions& options, CrtPreset stored) {
    if (!options.original_video_output)
        return;
    const std::string label(crt_preset_label(stored));
    log_original_video_output(jfg::LogLevel::info, "CRT filter " + label +
        (n64_video::crt_filter_active(options)
            ? "; the VI pass scales the frame through it instead of RT64's pixel-scaling filter"
            : ""));
    if (n64_video::crt_shader_curvature(options) != 0)
        log_original_video_output(jfg::LogLevel::info, "Screen curvature on; the CRT filter bends the picture");
}

// Applies JFG's video settings to RT64's user configuration. Call it before
// RT64::Application::setup, after the shell has installed its render hooks.
inline void apply_video_options(const Rt64ShellConfiguration& configuration,
                                RT64::UserConfiguration& user_configuration) {
    n64_video::VideoOptions options;
    options.original_video_output = configuration.video_output == VideoOutput::original;
    options.crt_preset = video_core_crt_preset(configuration.crt_filter);
    options.crt_curvature = configuration.crt_curvature;
    // The core reports coverage statuses and the VI pre-pass's gamma dither
    // whenever Original video output runs, with or without a coverage estimate.
    if (options.original_video_output) {
        n64_video::log_sink() = [](n64_video::LogLevel level, const char* message) {
            log_original_video_output(video_core_log_level(level), message);
        };
    }
    if (n64_video::estimates_coverage(options)) {
        log_original_video_output(jfg::LogLevel::info,
            "VI anti-aliasing and divot filter from estimated coverage; "
            "MSAA resolves take RT64's raster path");
    }
    log_crt_filter(options, configuration.crt_filter);
    n64_video::apply_to_rt64(options, user_configuration);
    limit_msaa_to_device_at_setup(user_configuration);
}

} // namespace jfg
