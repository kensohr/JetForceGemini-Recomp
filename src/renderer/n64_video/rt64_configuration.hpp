#pragma once
#include "msaa.hpp"
#include "video_options.hpp"

namespace n64_video {

// RT64's anti-aliasing setting for an MSAA sample count this core picks: 8, 4
// or 1 (MSAA off).
//
// UserConfiguration is RT64::UserConfiguration. It is a template parameter so
// this header needs no RT64 include path and can be tested without RT64.
template <class UserConfiguration>
typename UserConfiguration::Antialiasing rt64_antialiasing(unsigned sample_count) {
    using Antialiasing = typename UserConfiguration::Antialiasing;
    switch (sample_count) {
    case msaa_preferred_sample_count:
        return Antialiasing::MSAA8X;
    case msaa_fallback_sample_count:
        return Antialiasing::MSAA4X;
    default:
        return Antialiasing::None;
    }
}

// Applies the options to RT64's user configuration and makes them the active
// options for the patched VI pass. Call it before RT64::Application::setup,
// which reads the configuration once.
template <class UserConfiguration>
void apply_to_rt64(const VideoOptions &options, UserConfiguration &configuration) {
    active_options() = options;
    if (!options.original_video_output)
        return;
    // Original renders at 1x. RT64's default AntiAliasedPixelScaling filter
    // and its VI window fit stay: the image fills the window height with
    // sharp pixels, and the aspect ratio still follows the game's VI setup.
    configuration.resolution = UserConfiguration::Resolution::Original;
    // The VI anti-aliases with 8 coverage sub-samples, so ask for 8x.
    // limit_msaa_to_device lowers it if the device can't do 8x.
    configuration.antialiasing =
        rt64_antialiasing<UserConfiguration>(msaa_preferred_sample_count);
}

// Lowers the MSAA that apply_to_rt64 requested to what the device supports
// (see msaa_sample_count) and returns the sample count now configured, 1 when
// MSAA is off. Call it once the device exists and before RT64 builds its
// multisampling: RT64's own check only turns an unsupported count off, and
// doesn't notice missing sample locations, which leaves the configuration
// claiming a count it doesn't render.
template <class UserConfiguration>
unsigned limit_msaa_to_device(UserConfiguration &configuration, MsaaSupport support) {
    if (configuration.antialiasing == UserConfiguration::Antialiasing::None)
        return msaa_off_sample_count;
    const unsigned sample_count = msaa_sample_count(support);
    configuration.antialiasing = rt64_antialiasing<UserConfiguration>(sample_count);
    return sample_count;
}

} // namespace n64_video
