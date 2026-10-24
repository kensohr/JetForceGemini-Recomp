#pragma once

namespace n64_video {

inline constexpr unsigned msaa_off_sample_count = 1;
inline constexpr unsigned msaa_preferred_sample_count = 8;
inline constexpr unsigned msaa_fallback_sample_count = 4;

// The MSAA sample count to render with: 8x, else 4x, else 1 (MSAA off). 8x
// matches the VI's 8 coverage sub-samples; 2x is too coarse to stand in for
// VI anti-aliasing, so it isn't offered.
//
// supported_sample_counts is the device's support for both the colour and the
// depth format, as a bit mask whose bit value is the count (1, 2, 4, 8), the
// form RT64's device reports. RT64 renders MSAA only with custom sample
// locations, so without them MSAA is off.
constexpr unsigned msaa_sample_count(unsigned supported_sample_counts,
                                     bool sample_locations_supported) {
    if (!sample_locations_supported)
        return msaa_off_sample_count;
    if (supported_sample_counts & msaa_preferred_sample_count)
        return msaa_preferred_sample_count;
    if (supported_sample_counts & msaa_fallback_sample_count)
        return msaa_fallback_sample_count;
    return msaa_off_sample_count;
}

} // namespace n64_video
