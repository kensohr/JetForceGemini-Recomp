#pragma once

// Project glue between the RT64 shell and the video core: maps JFG's shell
// configuration onto the core's options. It lives outside the core, which
// must not know any one game's types.
#include "jfg/renderer/rt64_shell.hpp"
#include "n64_video/rt64_configuration.hpp"
#include "rt64_msaa_support.hpp"
#include "vi_anti_aliasing_switch.hpp"

#include <iostream>

namespace jfg {

inline n64_video::VideoOptions video_core_options(
    const Rt64ShellConfiguration& configuration,
    n64_video::AntiAliasing anti_aliasing) noexcept {
    n64_video::VideoOptions options;
    options.original_video_output =
        configuration.video_output == VideoOutput::original;
    options.anti_aliasing = anti_aliasing;
    return options;
}

// Applies JFG's video settings to RT64's user configuration. Call it before
// RT64::Application::setup, after the shell has installed its render hooks.
inline void apply_video_options(const Rt64ShellConfiguration& configuration,
                                RT64::UserConfiguration& user_configuration) {
    const bool is_original = configuration.video_output == VideoOutput::original;
    // The switch only affects Original, so Enhanced never reads or warns about it.
    const n64_video::AntiAliasing anti_aliasing = is_original
        ? read_vi_anti_aliasing_switch(std::cerr)
        : n64_video::AntiAliasing::off;
    if (is_original && anti_aliasing == n64_video::AntiAliasing::off)
        write_log(std::cerr, LogLevel::info,
            "Original video output: MSAA off (JFG_VI_AA=off)");
    n64_video::apply_to_rt64(video_core_options(configuration, anti_aliasing),
        user_configuration);
    limit_msaa_to_device_at_setup(user_configuration);
}

} // namespace jfg
