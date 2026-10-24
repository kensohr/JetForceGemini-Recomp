#pragma once

// Project glue between the RT64 shell and the video core: maps JFG's shell
// configuration onto the core's options. It lives outside the core, which
// must not know any one game's types.
#include "jfg/renderer/rt64_shell.hpp"
#include "n64_video/coverage_estimate.hpp"
#include "n64_video/rt64_configuration.hpp"
#include "rt64_msaa_support.hpp"
#include "vi_anti_aliasing_switch.hpp"

#include <iostream>

namespace jfg {

// Applies JFG's video settings to RT64's user configuration. Call it before
// RT64::Application::setup, after the shell has installed its render hooks.
inline void apply_video_options(const Rt64ShellConfiguration& configuration,
                                RT64::UserConfiguration& user_configuration) {
    n64_video::VideoOptions options;
    options.original_video_output = configuration.video_output == VideoOutput::original;
    // The switch only affects Original, so Enhanced never reads or warns about it.
    options.anti_aliasing = options.original_video_output
        ? read_vi_anti_aliasing_switch(std::cerr)
        : n64_video::AntiAliasing::off;
    if (options.original_video_output && options.anti_aliasing == n64_video::AntiAliasing::off)
        log_original_video_output(LogLevel::info, "MSAA off (JFG_VI_AA=off)");
    if (n64_video::estimates_coverage(options)) {
        log_original_video_output(LogLevel::info,
            "VI anti-aliasing and divot filter from estimated coverage (JFG_VI_AA=coverage)");
        n64_video::coverage_log_sink() = [](const char* message) {
            log_original_video_output(LogLevel::debug, message);
        };
    }
    n64_video::apply_to_rt64(options, user_configuration);
    limit_msaa_to_device_at_setup(user_configuration);
}

} // namespace jfg
