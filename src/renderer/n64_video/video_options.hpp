#pragma once

// Video core: reusable N64 video output for RT64-based recompilations.
// Code in this folder must not depend on any one game or launcher; a project
// configures it only through VideoOptions.
#include <cstdint>

namespace n64_video {

// The display the CRT filter simulates on top of Original video output (see
// crt_filter.hpp), or Off. The values are the CRT shader's preset ids.
enum class CrtPreset : std::uint32_t {
    off = 0,
    rgb_monitor = 1,
};

struct VideoOptions {
    // Show the game as the console sent it to the TV: native resolution
    // (320x240) and the VI's 16-bit colour, instead of a window-scaled image.
    // Under it, RT64 renders with MSAA at the RDP's sample positions; each
    // pixel's RDP coverage and front-surface colour are estimated from the
    // samples and each draw's render mode, and the VI pass runs the VI's own
    // anti-aliasing and divot filter on them.
    bool original_video_output = false;
    // Under Original video output, the VI pass draws the frame through this
    // CRT preset instead of RT64's pixel-scaling filter. Ignored otherwise.
    CrtPreset crt_preset = CrtPreset::off;
    // Under a CRT preset, the CRT pass bends the picture like a CRT tube,
    // with a vignette and rounded corners. Ignored without a CRT preset.
    bool crt_curvature = false;
};

// The options in effect for the running renderer. apply_to_rt64 sets them;
// the patched RT64 VI pass reads them on its presentation thread, which RT64
// starts after setup, so no further synchronisation is needed. Set them only
// before RT64::Application::setup: a later change races with that thread.
inline VideoOptions &active_options() {
    static VideoOptions options;
    return options;
}

// Whether RT64 estimates per-pixel coverage for the VI pass: always under
// Original video output, which needs it for VI anti-aliasing and the divot
// filter.
constexpr bool estimates_coverage(const VideoOptions &options) {
    return options.original_video_output;
}

} // namespace n64_video
