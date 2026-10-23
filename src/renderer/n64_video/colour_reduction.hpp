#pragma once
#include "video_options.hpp"
#include <cstdint>

namespace n64_video {

// RDP colour-dither patterns, numbered as in the RDP's other-mode colour
// dither field (and RT64's DitherPatternValue).
enum class DitherPattern : std::uint32_t {
    magic_square = 0,
    bayer = 1,
};

// How the VI pass reduces a presented frame to the colour depth the console
// scanned out.
struct ColourReduction {
    // Reduce each framebuffer pixel to RGBA5551 with the dither pattern.
    bool enabled = false;
    DitherPattern pattern = DitherPattern::magic_square;
};

// VI_CONTROL (VI_STATUS) bits 1:0 hold the framebuffer colour type.
inline constexpr std::uint32_t vi_control_type_mask = 0x3U;
inline constexpr std::uint32_t vi_control_type_16_bit = 0x2U;

// Original video output reduces a 16-bit framebuffer the way the RDP writes
// one: each 8-bit channel plus the pattern value (0-7), clamped, top 5 bits.
// A 32-bit or blank framebuffer is shown as rendered.
//
// The pattern is fixed: dithering once per presented frame approximates the
// RDP dithering every blend with the pattern each draw selects. Magic square
// is libultra's default and the one games normally select.
inline ColourReduction colour_reduction(const VideoOptions &options,
                                        std::uint32_t vi_control) {
    const bool is_16_bit_framebuffer =
        (vi_control & vi_control_type_mask) == vi_control_type_16_bit;
    ColourReduction reduction;
    reduction.enabled = options.original_video_output && is_16_bit_framebuffer;
    return reduction;
}

} // namespace n64_video
