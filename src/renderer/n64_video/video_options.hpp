#pragma once

// Video core: reusable N64 video output for RT64-based recompilations.
// Code in this folder must not depend on any one game or launcher; a project
// configures it only through VideoOptions.
namespace n64_video {

struct VideoOptions {
    // Show the game at its native resolution (320x240) instead of a
    // window-scaled one.
    bool original_video_output = false;
};

} // namespace n64_video
