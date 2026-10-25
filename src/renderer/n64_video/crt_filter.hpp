#pragma once
#include "crt_filter.h"
#include "video_options.hpp"
#include <cstdint>

// Video core CRT filter: simulates the display the console was connected to,
// on top of Original video output. The patched RT64 VI pass runs it as its
// final draw, in place of RT64's pixel-scaling filter, on the VI pre-pass's
// native-resolution output (CrtFilter.hlsli). It fills the same picture
// rectangle and runs on every presented frame, interpolated ones included.
namespace n64_video {

static_assert(std::uint32_t(CrtPreset::off) == crt::CrtPresetOff);
static_assert(std::uint32_t(CrtPreset::rgb_monitor) == crt::CrtPresetRgbMonitor);
static_assert(std::uint32_t(CrtPreset::composite_tv) == crt::CrtPresetCompositeTv);
static_assert(std::uint32_t(CrtPreset::svideo_tv) == crt::CrtPresetSvideoTv);

// Whether the VI pass draws through the CRT filter: only under Original video
// output, whose native-resolution frame it needs for clean scanlines.
constexpr bool crt_filter_active(const VideoOptions &options) {
    return options.original_video_output && (options.crt_preset != CrtPreset::off);
}

// The CRT shader's preset id for these options; CrtPresetOff when the filter
// doesn't run.
constexpr std::uint32_t crt_shader_preset(const VideoOptions &options) {
    return crt_filter_active(options) ? std::uint32_t(options.crt_preset) : crt::CrtPresetOff;
}

} // namespace n64_video
