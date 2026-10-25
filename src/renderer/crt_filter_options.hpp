#pragma once

// Project glue: maps JFG's CRT filter setting onto the video core's CRT
// preset. It lives outside the core, which must not know any one game's types.
#include "jfg/renderer/crt_preset.hpp"
#include "n64_video/video_options.hpp"

namespace jfg {

constexpr n64_video::CrtPreset video_core_crt_preset(CrtPreset preset) {
    switch (preset) {
    case CrtPreset::rgb_monitor:
        return n64_video::CrtPreset::rgb_monitor;
    case CrtPreset::composite_tv:
        return n64_video::CrtPreset::composite_tv;
    case CrtPreset::svideo_tv:
        return n64_video::CrtPreset::svideo_tv;
    case CrtPreset::off:
        break;
    }
    return n64_video::CrtPreset::off;
}

} // namespace jfg
