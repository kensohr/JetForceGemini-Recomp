#pragma once

// Project glue between the RT64 shell and the video core: maps JFG's shell
// configuration onto the core's options. It lives outside the core, which
// must not know any one game's types.
#include "jfg/renderer/rt64_shell.hpp"
#include "n64_video/rt64_configuration.hpp"

namespace jfg {

inline n64_video::VideoOptions video_core_options(
    const Rt64ShellConfiguration& configuration) noexcept {
    n64_video::VideoOptions options;
    options.original_video_output =
        configuration.video_output == VideoOutput::original;
    return options;
}

} // namespace jfg
