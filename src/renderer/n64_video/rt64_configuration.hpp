#pragma once
#include "video_options.hpp"

namespace n64_video {

// Applies the options to RT64's user configuration and makes them the active
// options for the patched VI pass. Call it before RT64::Application::setup,
// which reads the configuration once.
//
// UserConfiguration is RT64::UserConfiguration. It is a template parameter so
// this header needs no RT64 include path and can be tested without RT64.
template <class UserConfiguration>
void apply_to_rt64(const VideoOptions &options, UserConfiguration &configuration) {
    active_options() = options;
    if (!options.original_video_output)
        return;
    // Original renders at 1x. RT64's default AntiAliasedPixelScaling filter
    // and its VI window fit stay: the image fills the window height with
    // sharp pixels, and the aspect ratio still follows the game's VI setup.
    configuration.resolution = UserConfiguration::Resolution::Original;
}

} // namespace n64_video
