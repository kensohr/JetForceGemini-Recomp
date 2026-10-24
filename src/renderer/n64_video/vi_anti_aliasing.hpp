#pragma once
#include "vi_filtering.hpp"
#include <algorithm>

// The VI's per-pixel filters that read coverage: VI anti-aliasing of edge
// pixels and the divot filter. These are the reference for the VI pre-pass
// shader, which repeats the same arithmetic per channel; the algorithms follow
// angrylion-rdp-plus (vi/fetch.c, vi/video.c, vi/divot.c) and parallel-RDP
// (vi_fetch.frag, vi_divot.frag), which agree.
namespace n64_video {

// Pixel coverage as the RDP stores it: the number of covered sub-samples
// minus one, 0-7.
inline constexpr unsigned full_coverage = 7;

// What the VI does to one fetched pixel before the divot filter.
enum class PixelFilter {
    // Blend with the background the VI reconstructs from the neighbours.
    anti_alias,
    dither_filter,
    none,
};

// The coverage the VI filters read: AA modes 2 and 3 treat every pixel as
// fully covered.
constexpr unsigned vi_coverage(unsigned stored_coverage, const ViFiltering &filtering) {
    return filtering.anti_aliasing ? stored_coverage : full_coverage;
}

// A partially covered pixel is anti-aliased; only a fully covered one gets
// the dither filter.
constexpr PixelFilter pixel_filter(unsigned stored_coverage, const ViFiltering &filtering) {
    if (vi_coverage(stored_coverage, filtering) < full_coverage)
        return PixelFilter::anti_alias;
    return filtering.dither_filter ? PixelFilter::dither_filter : PixelFilter::none;
}

// Neighbour lists are any range of int (an array, a std::array); the core is
// C++17 because RT64 includes it.

// The second-largest of the centre, counted twice, and the fully covered
// neighbours. Counting the centre twice is how the VI seeds its search, so
// the result is never below the centre.
template <class Values>
int penultimate_max(int centre, const Values &full_neighbours) {
    int largest = centre;
    int second = centre;
    for (const int value : full_neighbours) {
        if (value > largest) {
            second = largest;
            largest = value;
        } else {
            second = std::max(second, value);
        }
    }
    return second;
}

// The second-smallest of the centre, counted twice, and the fully covered
// neighbours; never above the centre.
template <class Values>
int penultimate_min(int centre, const Values &full_neighbours) {
    int smallest = centre;
    int second = centre;
    for (const int value : full_neighbours) {
        if (value < smallest) {
            second = smallest;
            smallest = value;
        } else {
            second = std::min(second, value);
        }
    }
    return second;
}

// One channel of VI anti-aliasing. The penultimate bounds of the fully
// covered neighbours at (-1,-1), (+1,-1), (-2,0), (+2,0), (-1,+1) and (+1,+1)
// estimate the background behind the edge; the pixel moves toward it by its
// missing coverage: c + (((pmin + pmax - 2c) * (7 - cvg) + 4) >> 3), with an
// arithmetic (flooring) shift. The result stays within 0-255 because both
// bounds bracket the centre.
template <class Values>
int vi_anti_alias_channel(int centre, const Values &full_neighbours, unsigned coverage) {
    const int background_offset = penultimate_min(centre, full_neighbours) +
                                  penultimate_max(centre, full_neighbours) - 2 * centre;
    const int missing_coverage = int(full_coverage) - int(coverage);
    return centre + ((background_offset * missing_coverage + 4) >> 3);
}

// The divot filter runs on a pixel when it or its left or right neighbour is
// partially covered (coverage as fetched, before VI anti-aliasing).
constexpr bool divot_applies(unsigned left_coverage, unsigned centre_coverage,
                             unsigned right_coverage) {
    return (left_coverage & centre_coverage & right_coverage) != full_coverage;
}

// One channel of the divot filter: the median of three.
constexpr int divot_median(int left, int centre, int right) {
    return std::max(std::min(left, centre), std::min(std::max(left, centre), right));
}

} // namespace n64_video
