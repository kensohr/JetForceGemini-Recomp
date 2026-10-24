#pragma once

// Video core: reusable N64 video output for RT64-based recompilations.
// Code in this folder must not depend on any one game or launcher; a project
// configures it only through VideoOptions.
namespace n64_video {

// How Original video output stands in for the VI's edge anti-aliasing.
enum class AntiAliasing {
    // Polygon edges stay as RT64 draws them: hard and stair-stepped.
    off,
    // RT64 renders with MSAA and the VI pass reads the averaged colour, so
    // edges are softened. An approximation: the VI blends only partially
    // covered pixels with its reconstructed background.
    msaa,
};

struct VideoOptions {
    // Show the game as the console sent it to the TV: native resolution
    // (320x240) and the VI's 16-bit colour, instead of a window-scaled image.
    bool original_video_output = false;
    // Applies only under original_video_output.
    AntiAliasing anti_aliasing = AntiAliasing::msaa;
};

// The options in effect for the running renderer. apply_to_rt64 sets them;
// the patched RT64 VI pass reads them on its presentation thread, which RT64
// starts after setup, so no further synchronisation is needed. Set them only
// before RT64::Application::setup: a later change races with that thread.
inline VideoOptions &active_options() {
    static VideoOptions options;
    return options;
}

} // namespace n64_video
