#pragma once
#include "video_options.hpp"
#include "vi_filtering.hpp"
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

inline constexpr std::uint32_t rdp_channel_low_bits_mask = 0x7U;
inline constexpr std::uint32_t rdp_channel_5_bit_shift = 3;
inline constexpr std::uint32_t rdp_channel_5_bit_max = 31;

// The RDP's dithered 16-bit write of one 8-bit channel: it rounds the channel
// up to the next 5-bit step when its low 3 bits exceed the pattern value
// (0-7), clamped at 31, else keeps the top 5 bits. This is the reference the
// VI pre-pass shader's ReduceToRGB5 implements.
constexpr std::uint32_t rdp_dither_channel(std::uint32_t channel8,
                                           std::uint32_t dither) {
    const bool rounds_up = (channel8 & rdp_channel_low_bits_mask) > dither;
    const std::uint32_t channel5 = channel8 >> rdp_channel_5_bit_shift;
    if (!rounds_up)
        return channel5;
    return (channel5 < rdp_channel_5_bit_max) ? channel5 + 1 : rdp_channel_5_bit_max;
}

// Original video output reduces a 16-bit framebuffer the way the RDP writes
// one (rdp_dither_channel). A 32-bit framebuffer keeps its 8-bit channels; a
// blank one shows nothing.
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
