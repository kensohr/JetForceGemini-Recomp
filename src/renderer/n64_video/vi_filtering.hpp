#pragma once
#include "video_options.hpp"
#include <cstdint>

namespace n64_video {

// The VI registers VI filtering reads, as the game last wrote them.
struct ViRegisters {
    std::uint32_t control = 0; // VI_CONTROL (VI_STATUS)
    std::uint32_t h_start = 0; // VI_H_START: start in bits 25:16, end in 9:0
    std::uint32_t x_scale = 0; // VI_X_SCALE: offset in bits 27:16, scale in 11:0
};

// How each output sample of a scanline is made from the framebuffer pixels
// around its position. Numbered as the VI pre-pass shader expects.
enum class Resampling : std::uint32_t {
    // Blend the two neighbouring pixels by the position's fraction.
    interpolate = 0,
    // Repeat the pixel the position falls in.
    replicate = 1,
};

// What the VI does to a framebuffer on its way to the TV, besides gamma: the
// dither filter, then horizontal resampling to the scanline.
struct ViFiltering {
    // Nudge each pixel toward its 3x3 neighbourhood to smooth dither grain.
    bool dither_filter = false;
    Resampling resampling = Resampling::interpolate;
    // Output samples per scanline (hEnd - hStart); 0 when nothing is shown.
    std::uint32_t active_width = 0;
    // Framebuffer pixels per output sample, and the first sample's position,
    // both 2.10 fixed point.
    std::uint32_t x_step = 0;
    std::uint32_t x_offset = 0;
};

// VI_CONTROL (VI_STATUS) bits 1:0 hold the framebuffer colour type: 0 blank,
// 1 reserved, 2 16-bit, 3 32-bit.
inline constexpr std::uint32_t vi_control_type_mask = 0x3U;
inline constexpr std::uint32_t vi_control_type_16_bit = 0x2U;
inline constexpr std::uint32_t vi_control_type_32_bit = 0x3U;

inline constexpr std::uint32_t vi_control_aa_mode_shift = 8;
inline constexpr std::uint32_t vi_control_aa_mode_mask = 0x3U;
// AA mode 3: no anti-aliasing and no resampling interpolation.
inline constexpr std::uint32_t vi_control_aa_mode_replicate = 3;
inline constexpr std::uint32_t vi_control_dither_filter_bit = 1U << 16;

inline constexpr std::uint32_t vi_h_start_field_mask = 0x3FFU;
inline constexpr std::uint32_t vi_h_start_start_shift = 16;
inline constexpr std::uint32_t vi_x_scale_field_mask = 0xFFFU;
inline constexpr std::uint32_t vi_x_scale_offset_shift = 16;

// Original video output runs the VI filtering whenever the VI scans out a
// 16- or 32-bit framebuffer. A blank or reserved colour type shows nothing.
inline bool vi_filtering_enabled(const VideoOptions &options,
                                 std::uint32_t vi_control) {
    const std::uint32_t type = vi_control & vi_control_type_mask;
    const bool shows_framebuffer =
        (type == vi_control_type_16_bit) || (type == vi_control_type_32_bit);
    return options.original_video_output && shows_framebuffer;
}

inline ViFiltering vi_filtering(const ViRegisters &registers) {
    const std::uint32_t aa_mode =
        (registers.control >> vi_control_aa_mode_shift) & vi_control_aa_mode_mask;
    const std::uint32_t h_start =
        (registers.h_start >> vi_h_start_start_shift) & vi_h_start_field_mask;
    const std::uint32_t h_end = registers.h_start & vi_h_start_field_mask;

    ViFiltering filtering;
    filtering.dither_filter = (registers.control & vi_control_dither_filter_bit) != 0;
    filtering.resampling = (aa_mode == vi_control_aa_mode_replicate)
                               ? Resampling::replicate
                               : Resampling::interpolate;
    filtering.active_width = (h_end > h_start) ? (h_end - h_start) : 0;
    filtering.x_step = registers.x_scale & vi_x_scale_field_mask;
    filtering.x_offset =
        (registers.x_scale >> vi_x_scale_offset_shift) & vi_x_scale_field_mask;
    return filtering;
}

} // namespace n64_video
