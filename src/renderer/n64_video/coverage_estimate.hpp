#pragma once
#include "coverage_layout.h"
#include "log_sink.hpp"
#include "vi_anti_aliasing.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

// Estimating the RDP's per-pixel coverage from MSAA samples, so the VI's own
// anti-aliasing and divot filter can run on it. These are the reference for
// the RT64 raster shader's per-sample record and the coverage estimate
// compute shader, which repeat the same rules.
//
// The RDP accumulates coverage per primitive as it draws. Here the estimate
// is made once from the finished samples instead: the front surface is the
// front-most sample by depth plus every sample close enough in depth to
// belong to the same surface, its coverage is the share of samples it covers,
// adjusted by the render mode of the draw that wrote them.
namespace n64_video {

// --- Sample positions -----------------------------------------------------

// The RDP samples each pixel at 8 points on 4 sub-scanlines, 2 per row:
// x 0 and 4/8 on rows 0 and 2, x 2/8 and 6/8 on rows 1 and 3, in eighths of a
// pixel from its top-left corner.
struct RdpSamplePoint {
    int x_eighths;
    int y_eighths;
};
inline constexpr std::array<RdpSamplePoint, 8> rdp_sample_points = {{
    {0, 0}, {4, 0}, {2, 2}, {6, 2}, {0, 4}, {4, 4}, {2, 6}, {6, 6},
}};
// With 4x MSAA, one sample per sub-scanline: (0,0), (6,2), (4,4), (2,6).
inline constexpr std::array<unsigned, 4> rdp_sample_subset_4x = {0, 3, 5, 6};

// One MSAA sample position, in RT64's convention: sixteenths of a pixel from
// the rendered pixel's centre, -8 to 7.
//
// RT64 draws RDP point (x, y) at rendered position (x + 0.5, y + 0.5), so
// rendered pixel (x, y)'s centre is RDP pixel (x, y)'s top-left corner. An
// RDP sample point k/8 to the right of that corner is 2k/16 from the centre;
// from 8/16 on it lies in the next rendered pixel, 16/16 back. The RDP's
// pattern is periodic, so every rendered pixel holds the same pattern, and
// RDP pixel (x, y)'s samples are spread over rendered pixels (x, y) to
// (x + 1, y + 1). next_pixel_x/y say which one holds this sample.
struct MsaaSamplePosition {
    int x_sixteenths;
    int y_sixteenths;
    unsigned next_pixel_x;
    unsigned next_pixel_y;
    constexpr bool operator==(const MsaaSamplePosition &other) const {
        return (x_sixteenths == other.x_sixteenths) && (y_sixteenths == other.y_sixteenths) &&
               (next_pixel_x == other.next_pixel_x) && (next_pixel_y == other.next_pixel_y);
    }
};

inline constexpr unsigned max_msaa_samples = 8;

struct MsaaSamplePattern {
    // 0 when there's no RDP pattern for the requested count.
    unsigned count = 0;
    std::array<MsaaSamplePosition, max_msaa_samples> positions{};
};

constexpr MsaaSamplePosition msaa_sample_position(RdpSamplePoint point) {
    constexpr int pixel_sixteenths = 16;
    constexpr int half_pixel_sixteenths = 8;
    const int x = 2 * point.x_eighths;
    const int y = 2 * point.y_eighths;
    const unsigned next_x = (x >= half_pixel_sixteenths) ? 1U : 0U;
    const unsigned next_y = (y >= half_pixel_sixteenths) ? 1U : 0U;
    return {x - int(next_x) * pixel_sixteenths, y - int(next_y) * pixel_sixteenths, next_x,
            next_y};
}

// The RDP's sample positions for 8x MSAA, or the 4-sample subset for 4x.
// Sample 0 is the RDP's top-left sample, the one a pixel without
// anti-aliasing is drawn by.
constexpr MsaaSamplePattern msaa_sample_pattern(unsigned sample_count) {
    MsaaSamplePattern pattern;
    if (sample_count == 8) {
        pattern.count = 8;
        for (unsigned s = 0; s < 8; s++)
            pattern.positions[s] = msaa_sample_position(rdp_sample_points[s]);
    } else if (sample_count == 4) {
        pattern.count = 4;
        for (unsigned s = 0; s < 4; s++)
            pattern.positions[s] = msaa_sample_position(rdp_sample_points[rdp_sample_subset_4x[s]]);
    }
    return pattern;
}

// The rendered pixel of each sample relative to its RDP pixel, packed for the
// estimate shader: bit 2s set when sample s lies one pixel to the right,
// bit 2s + 1 when it lies one pixel down.
constexpr std::uint32_t sample_pixel_offset_bits(const MsaaSamplePattern &pattern) {
    std::uint32_t bits = 0;
    for (unsigned s = 0; s < pattern.count; s++) {
        bits |= pattern.positions[s].next_pixel_x << (2 * s);
        bits |= pattern.positions[s].next_pixel_y << (2 * s + 1);
    }
    return bits;
}

// The samples that lie in the RDP pixel's own rendered pixel, shaded at its
// position.
constexpr unsigned own_pixel_mask(const MsaaSamplePattern &pattern) {
    unsigned mask = 0;
    for (unsigned s = 0; s < pattern.count; s++) {
        const MsaaSamplePosition &position = pattern.positions[s];
        if ((position.next_pixel_x == 0) && (position.next_pixel_y == 0))
            mask |= 1U << s;
    }
    return mask;
}

// --- Per-sample render-mode record ----------------------------------------

// The other mode's CVG_DST field: what a draw does with the coverage it
// computes. Numbered as the field's value (bits 9:8).
enum class CoverageDestination : unsigned {
    clamp = 0,
    wrap = 1,
    full = 2,
    save = 3,
};

// The parts of a draw's other mode that decide what it does with coverage.
struct DrawCoverageMode {
    CoverageDestination destination = CoverageDestination::clamp;
    // AA_EN: edges get partial coverage.
    bool anti_aliased = false;
    // CVG_X_ALPHA: coverage is multiplied by alpha.
    bool coverage_times_alpha = false;
    // Copy and fill cycles write the pixel whole.
    bool copy_or_fill = false;
};

// What the estimate reads from one MSAA sample.
struct SampleRecord {
    // The draw anti-aliases: the pixel's coverage is the share of samples it
    // covers. Otherwise the pixel is drawn whole when its top-left sample is
    // covered.
    bool partial = false;
    // Coverage the sample contributes, in eighths of the sample (0-8): 8, or
    // alpha * 8 under CVG_X_ALPHA.
    unsigned eighths = 8;
    constexpr bool operator==(const SampleRecord &other) const {
        return (partial == other.partial) && (eighths == other.eighths);
    }
};

inline constexpr unsigned sample_full_eighths = coverage_layout::SampleFullEighths;

// The record rides in the colour target's alpha with RT64's own coverage
// value, which stays exactly as RT64 computes it (coverage_layout.h). RT64
// already stores the sample's eighths there (7 for a full sample,
// round(8 * alpha) under CVG_X_ALPHA, CVG_DST_WRAP adding, CVG_DST_SAVE
// keeping); the record adds only whether the draw anti-aliases.
//
// Whether a draw marks its samples as anti-aliased: AA_EN with
// CVG_DST_CLAMP, except copy and fill. CVG_DST_FULL, draws without AA_EN,
// copy and fill write a full pixel. CVG_DST_SAVE and CVG_DST_WRAP add nothing:
// RT64 blends these destinations' alpha additively, so the record of the
// draw under them stays. The coverage patch's CoverageRecord.hlsli repeats
// this rule on RT64's other mode.
constexpr bool records_partial(const DrawCoverageMode &mode) {
    return (mode.destination == CoverageDestination::clamp) && mode.anti_aliased && !mode.copy_or_fill;
}

// The alpha value the raster shader writes for a sample, from RT64's own
// output for it (in units of the alpha).
constexpr unsigned record_sample_alpha(unsigned rt64_output, const DrawCoverageMode &mode) {
    return records_partial(mode) ? rt64_output + coverage_layout::PartialRecordMarker : rt64_output;
}

// RT64's coverage value of a stored alpha value, the record taken off.
// `full_value` is the alpha range (255, or 65535 with HDR).
constexpr unsigned rt64_coverage_value(unsigned stored, unsigned full_value) {
    return coverage_layout::Rt64CoverageValue(stored, full_value);
}

// What the estimate reads from a stored alpha value.
constexpr SampleRecord decode_sample_record(unsigned stored, unsigned full_value) {
    return SampleRecord{coverage_layout::IsPartialRecord(stored, full_value),
                        coverage_layout::SampleEighths(rt64_coverage_value(stored, full_value))};
}
// --- Front surface ----------------------------------------------------------

// The front-most depth of the pixel and of its four neighbours (each the
// nearest of its samples).
struct DepthNeighbours {
    float centre;
    float left;
    float right;
    float up;
    float down;
};

// The RDP blends a new surface with the stored one, rather than drawing it in
// front, when their depths are within 8 * dz, dz being the larger depth slope
// per pixel (|dz/dx| + |dz/dy|). Samples within that tolerance of the
// front-most one count as the same surface, so the inner edges of a mesh stay
// fully covered. The slope is estimated from the neighbouring pixels: per
// axis the smaller of the two differences, so a silhouette on one side
// doesn't widen it. Thin geometry with a different surface on both sides gets
// a wide tolerance and counts as fully covered.
inline constexpr float rdp_depth_tolerance_factor = 8.0f;
// The RDP's smallest dz is one unit of its 18-bit depth; 8 of them keep a
// flat surface's samples together despite float rounding.
inline constexpr float minimum_depth_tolerance = rdp_depth_tolerance_factor / 262143.0f;

inline float front_depth_tolerance(const DepthNeighbours &depth) {
    const float slope_x = std::min(std::fabs(depth.centre - depth.left),
                                   std::fabs(depth.right - depth.centre));
    const float slope_y = std::min(std::fabs(depth.centre - depth.up),
                                   std::fabs(depth.down - depth.centre));
    return rdp_depth_tolerance_factor * (slope_x + slope_y) + minimum_depth_tolerance;
}

// The samples of the front surface: those within the tolerance of the
// front-most depth.
template <class Depths>
unsigned front_surface_mask(const Depths &sample_depths, float tolerance) {
    const auto nearest = std::min_element(std::begin(sample_depths), std::end(sample_depths));
    if (nearest == std::end(sample_depths))
        return 0;
    const float front = *nearest;
    unsigned mask = 0;
    for (std::size_t s = 0; s < std::size(sample_depths); s++) {
        if (sample_depths[s] <= front + tolerance)
            mask |= 1U << s;
    }
    return mask;
}

// --- Pixel coverage -----------------------------------------------------------

struct PixelCoverage {
    // As the RDP stores it: covered sub-samples minus one, 0-7.
    unsigned coverage = full_coverage;
    // The samples whose average is the pixel's colour. The background never
    // mixes in: the VI reconstructs it from the neighbouring pixels.
    unsigned colour_mask = 1;
};

// The pixel's coverage and colour samples from its front surface.
// - An anti-aliased front (every front sample partial) covers the share of
//   the pixel its samples cover, each scaled by its alpha under CVG_X_ALPHA.
//   Its colour is the front samples in the RDP pixel's own rendered pixel, or
//   all front samples when none lies there.
// - Otherwise the RDP draws the pixel whole if its top-left sample is
//   covered: the pixel is sample 0, with sample 0's coverage (alpha-scaled
//   under CVG_X_ALPHA, full when sample 0 belongs to an anti-aliased surface
//   behind).
// `records` holds one SampleRecord per sample of the pattern.
template <class Records>
PixelCoverage pixel_coverage(const Records &records, const MsaaSamplePattern &pattern,
                             unsigned front_mask) {
    bool front_partial = front_mask != 0;
    unsigned front_eighths = 0;
    for (unsigned s = 0; s < pattern.count; s++) {
        if ((front_mask & (1U << s)) == 0)
            continue;
        front_partial = front_partial && records[s].partial;
        front_eighths += records[s].eighths;
    }

    PixelCoverage result;
    unsigned covered_eighths = 0;
    if (front_partial) {
        // Each sample stands for 8 / count eighths of the pixel, scaled by
        // its own eighths: sum(eighths) / count, rounded.
        covered_eighths = (front_eighths + pattern.count / 2) / pattern.count;
        const unsigned own_front = front_mask & own_pixel_mask(pattern);
        result.colour_mask = (own_front != 0) ? own_front : front_mask;
    } else {
        const SampleRecord &top_left = records[0];
        covered_eighths = top_left.partial ? sample_full_eighths : top_left.eighths;
        result.colour_mask = 1;
    }
    result.coverage = std::max(std::min(covered_eighths, sample_full_eighths), 1U) - 1;
    return result;
}

// --- Availability and diagnostics ----------------------------------------------

// Whether a frame estimates coverage, and if not, why; it then shows the
// averaged colour at full coverage.
enum class CoverageStatus : unsigned {
    estimating,
    // MSAA off, or without the RDP's sample positions.
    no_msaa,
    // The estimate reads one rendered pixel per RDP pixel.
    scaled_resolution,
    downsampled,
    // A framebuffer pair without a depth target and no front surface from
    // an earlier pair of the same frame: every sample counts as front, so
    // the coverage comes from the draws' records alone. Set by the RT64 side,
    // which knows the pair.
    no_depth,
    count,
};

struct CoverageTarget {
    unsigned sample_count = 1;
    bool rdp_sample_positions = false;
    float resolution_scale_x = 1.0f;
    float resolution_scale_y = 1.0f;
    unsigned downsample_multiplier = 1;
};

inline CoverageStatus coverage_status(const CoverageTarget &target) {
    if ((msaa_sample_pattern(target.sample_count).count == 0) || !target.rdp_sample_positions)
        return CoverageStatus::no_msaa;
    if ((target.resolution_scale_x != 1.0f) || (target.resolution_scale_y != 1.0f))
        return CoverageStatus::scaled_resolution;
    if (target.downsample_multiplier > 1)
        return CoverageStatus::downsampled;
    return CoverageStatus::estimating;
}

// A running estimate is routine; every other status is a fallback.
constexpr LogLevel coverage_status_log_level(CoverageStatus status) {
    return (status == CoverageStatus::estimating) ? LogLevel::debug : LogLevel::warning;
}

// The message the RT64 side reports to log_sink(), once per status and run,
// with the values that decided it.
inline std::string coverage_status_message(CoverageStatus status, const CoverageTarget &target) {
    const std::string samples = std::to_string(target.sample_count) + "x MSAA";
    const std::string fallback = "; showing the averaged colour at full coverage";
    switch (status) {
    case CoverageStatus::estimating:
        return "coverage estimate running (" + samples + " at the RDP's sample positions)";
    case CoverageStatus::no_msaa:
        return "coverage unavailable with " + samples +
               (target.rdp_sample_positions ? "" : " without the RDP's sample positions") +
               "; it needs 8x or 4x MSAA with sample locations" + fallback;
    case CoverageStatus::scaled_resolution: {
        char scale[64];
        std::snprintf(scale, sizeof(scale), "%.2f x %.2f", double(target.resolution_scale_x),
                      double(target.resolution_scale_y));
        return std::string("coverage unavailable at resolution scale ") + scale +
               "; it needs 1 x 1" + fallback;
    }
    case CoverageStatus::downsampled:
        return "coverage unavailable with downsample multiplier " +
               std::to_string(target.downsample_multiplier) + "; it needs 1" + fallback;
    case CoverageStatus::no_depth:
        return "framebuffer pair without a depth target (" + samples +
               "); coverage from the draws' render modes only";
    case CoverageStatus::count:
        break;
    }
    return "";
}

} // namespace n64_video
