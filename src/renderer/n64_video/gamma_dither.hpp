#pragma once
#include "vi_filtering.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

// The VI's gamma-dither noise without gamma: the last step of the VI, after
// resampling, adds a random 0 or 1 to each 8-bit channel of each output
// sample, independently per channel, leaving a channel at 255 as it is. This
// is the reference for the VI pre-pass shader, which repeats the same
// arithmetic; the step follows angrylion-rdp-plus (vi/video.c,
// vi_process_full) and parallel-RDP (vi_scale.frag).
namespace n64_video {

inline constexpr int gamma_dither_channel_max = 255;

// One channel plus its noise bit (0 or 1), saturating at 255.
constexpr int gamma_dither_channel(int value, std::uint32_t noise_bit) {
    return std::min(value + int(noise_bit), gamma_dither_channel_max);
}

// Chris Wellons' lowbias32 integer hash: every input bit changes about half
// the output bits. 32-bit unsigned arithmetic wraps the same way in HLSL.
constexpr std::uint32_t gamma_dither_hash(std::uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352DU;
    value ^= value >> 15;
    value *= 0x846CA68BU;
    value ^= value >> 16;
    return value;
}

inline constexpr std::uint32_t gamma_dither_bits_mask = 0x7U;

// The noise bits of one output sample (x along the active scanline, y the
// framebuffer row) in one presented frame: bit 0 red, bit 1 green, bit 2
// blue. Stateless, so the shader needs no generator state; chaining the hash
// through frame, y and x decorrelates neighbours, channels and frames. The
// hardware generator's own sequence isn't reproduced.
constexpr std::uint32_t gamma_dither_bits(std::uint32_t x, std::uint32_t y, std::uint32_t frame) {
    const std::uint32_t hash = gamma_dither_hash(x + gamma_dither_hash(y + gamma_dither_hash(frame)));
    return hash & gamma_dither_bits_mask;
}

// Where the RT64 side reports a gamma-dither setting it can't show, as a
// warning. A project sets it before RT64 starts rendering; without one, the
// report is dropped.
using GammaDitherLogSink = void (*)(const char *message);
inline GammaDitherLogSink &gamma_dither_log_sink() {
    static GammaDitherLogSink sink = nullptr;
    return sink;
}

inline std::string gamma_dither_message(std::uint32_t vi_control) {
    char value[16];
    std::snprintf(value, sizeof(value), "0x%X", unsigned(vi_control));
    return std::string("VI_CONTROL ") + value +
           " sets gamma dither with gamma; its noise feeds the gamma table at 1/64 step, "
           "which the VI pass's gamma after the 8-bit pre-pass can't take, so no noise is added";
}

// Warns once per run when VI_CONTROL asks for gamma dither with gamma; every
// other setting is shown as asked, so it isn't reported. `setting` is what
// vi_filtering decided from `vi_control`, which only goes into the message.
// While no sink is set, a report is dropped without using up the warning.
inline void report_gamma_dither(GammaDither setting, std::uint32_t vi_control) {
    if (setting != GammaDither::unsupported_with_gamma)
        return;
    const GammaDitherLogSink sink = gamma_dither_log_sink();
    if (sink == nullptr)
        return;
    static std::atomic<bool> reported{false};
    if (reported.exchange(true))
        return;
    sink(gamma_dither_message(vi_control).c_str());
}

} // namespace n64_video
