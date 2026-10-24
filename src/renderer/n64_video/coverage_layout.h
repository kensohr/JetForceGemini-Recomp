//
// Video core coverage estimate: the values the core's reference
// (coverage_estimate.hpp) and the coverage patch's shaders share, defined
// once. C++ includes this file directly; the coverage patch copies it to
// RT64's src/shared/rt64_coverage_layout.h for the shaders. It is written in
// the subset C++ and HLSL have in common; __HLSL_VERSION (defined by DXC)
// tells them apart.
//

#pragma once

#ifdef __HLSL_VERSION
#define N64_VIDEO_LAYOUT_CONSTANT static const
#define N64_VIDEO_LAYOUT_FUNCTION
#else
namespace n64_video::coverage_layout {
using uint = unsigned;
#define N64_VIDEO_LAYOUT_CONSTANT inline constexpr
#define N64_VIDEO_LAYOUT_FUNCTION inline constexpr
#endif

// RT64 keeps its own coverage value in the colour target's alpha, in units of
// the alpha (1/255, or 1/65535 with HDR). It writes bit 2 of the low 3 bits
// back to RDRAM as a 16-bit pixel's coverage bit, after averaging a pixel's
// samples.
N64_VIDEO_LAYOUT_CONSTANT uint Rt64CoverageBits = 3;
// RT64's value for a fully covered sample: CVG_DST_FULL, or CVG_DST_CLAMP
// without CVG_X_ALPHA.
N64_VIDEO_LAYOUT_CONSTANT uint Rt64FullSampleValue = (1U << Rt64CoverageBits) - 1;

// The per-sample record: an anti-aliased draw adds this marker to RT64's
// value in the samples it covers. RT64's own values stay far below it (they
// pass 127 only after 16 full-coverage CVG_DST_WRAP draws on one sample), so
// the marker can be taken off again exactly: per sample before RT64 averages
// a pixel (the coverage patch's raster resolve), and by the estimate.
N64_VIDEO_LAYOUT_CONSTANT uint PartialRecordMarker = 128;

// A sample covers 8 eighths of itself; with CVG_X_ALPHA, round(8 * alpha).
N64_VIDEO_LAYOUT_CONSTANT uint SampleFullEighths = 8;

// The estimate texture (RGBA8) stores the RDP coverage (0-7) in its alpha as
// coverage / EstimateCoverageScale.
N64_VIDEO_LAYOUT_CONSTANT float EstimateCoverageScale = 255.0f;

// Whether a stored alpha value carries the partial record. `fullValue` is the
// alpha range (255 or 65535): a full alpha comes from a clear or an RDRAM load
// and carries no record.
N64_VIDEO_LAYOUT_FUNCTION bool IsPartialRecord(uint value, uint fullValue) {
    return (value >= PartialRecordMarker) && (value < 2U * PartialRecordMarker) && (value != fullValue);
}

// RT64's own coverage value of a stored alpha value, without the record.
N64_VIDEO_LAYOUT_FUNCTION uint Rt64CoverageValue(uint value, uint fullValue) {
    return IsPartialRecord(value, fullValue) ? (value - PartialRecordMarker) : value;
}

// The eighths of the sample its front draw covered, from RT64's value. 0
// (nothing drawn: a clear or an RDRAM load) and RT64's full value or more
// (CVG_DST_WRAP adds) count as the whole sample.
N64_VIDEO_LAYOUT_FUNCTION uint SampleEighths(uint rt64Value) {
    return ((rt64Value == 0U) || (rt64Value >= Rt64FullSampleValue)) ? SampleFullEighths : rt64Value;
}

#ifndef __HLSL_VERSION
} // namespace n64_video::coverage_layout
#endif

#undef N64_VIDEO_LAYOUT_CONSTANT
#undef N64_VIDEO_LAYOUT_FUNCTION
