// Video core checks for VI anti-aliasing and the divot filter from estimated
// coverage (option B of Original video output's anti-aliasing).
#include "n64_video/coverage_estimate.hpp"
#include "n64_video/vi_anti_aliasing.hpp"
#include "n64_video/vi_filtering.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <iterator>

int main() {
  unsigned failures = 0;
  auto check = [&](bool ok, const char *name) {
    if (!ok) {
      ++failures;
      std::cerr << "FAIL: " << name << '\n';
    }
  };

  // VI registers as libultra's NTSC low-res mode sets them (see
  // n64_video_tests.cpp). VI_CONTROL 0x13016: divot on (bit 4), dither filter
  // on, AA mode 0 (bits 9:8), 16-bit.
  constexpr std::uint32_t vi_h_start_ntsc = 0x006C02ECU;
  constexpr std::uint32_t vi_x_scale_320 = 0x00000200U;
  auto filtering = [&](std::uint32_t control) {
    return n64_video::vi_filtering({control, vi_h_start_ntsc, vi_x_scale_320});
  };
  check(filtering(0x13016U).anti_aliasing, "AA mode 0 anti-aliases edge pixels");
  check(filtering(0x13116U).anti_aliasing, "AA mode 1 anti-aliases edge pixels");
  check(!filtering(0x13216U).anti_aliasing, "AA mode 2 treats every pixel as fully covered");
  check(!filtering(0x13316U).anti_aliasing, "AA mode 3 treats every pixel as fully covered");
  check(filtering(0x13016U).divot, "VI_CONTROL bit 4 turns the divot filter on");
  check(!filtering(0x13006U).divot, "the divot filter stays off with bit 4 clear");

  // Each fetched pixel gets VI anti-aliasing when partially covered, else the
  // dither filter when it is on (angrylion vi_fetch_filter16).
  using n64_video::PixelFilter;
  const auto game = filtering(0x13016U);
  check(n64_video::pixel_filter(6, game) == PixelFilter::anti_alias,
        "a partially covered pixel is anti-aliased");
  check(n64_video::pixel_filter(0, game) == PixelFilter::anti_alias,
        "a pixel with the least coverage is anti-aliased");
  check(n64_video::pixel_filter(7, game) == PixelFilter::dither_filter,
        "a fully covered pixel gets the dither filter");
  check(n64_video::pixel_filter(7, filtering(0x03016U)) == PixelFilter::none,
        "a fully covered pixel stays as it is without the dither filter");
  check(n64_video::pixel_filter(3, filtering(0x03016U)) == PixelFilter::anti_alias,
        "anti-aliasing doesn't depend on the dither filter");
  check(n64_video::pixel_filter(3, filtering(0x13216U)) == PixelFilter::dither_filter,
        "AA mode 2 dither-filters a partially covered pixel as if fully covered");
  check(n64_video::vi_coverage(3, filtering(0x13316U)) == 7 &&
            n64_video::vi_coverage(3, game) == 3,
        "AA modes 2 and 3 read every pixel as fully covered");

  // VI anti-aliasing: per channel, the penultimate maximum and minimum of the
  // centre and its fully covered neighbours, with the centre counted twice
  // (angrylion video_max_optimized, parallel-RDP vi_fetch.frag seed both
  // bounds with the centre). Worked: centre 100 and neighbours 200, 50, 120
  // give the values 100 100 200 50 120: penultimate max 120, min 100.
  const int three_neighbours[] = {200, 50, 120};
  check(n64_video::penultimate_max(100, three_neighbours) == 120,
        "penultimate max of the centre twice and the neighbours");
  check(n64_video::penultimate_min(100, three_neighbours) == 100,
        "penultimate min of the centre twice and the neighbours");
  const std::array<int, 0> no_neighbours{};
  const int one_lower[] = {40};
  check(n64_video::penultimate_max(90, one_lower) == 90 &&
            n64_video::penultimate_min(90, one_lower) == 90,
        "a single neighbour can't move the bounds past the centre");
  check(n64_video::penultimate_max(90, no_neighbours) == 90 && n64_video::penultimate_min(90, no_neighbours) == 90,
        "with no fully covered neighbour both bounds are the centre");
  const int duplicates[] = {30, 30, 200};
  check(n64_video::penultimate_min(100, duplicates) == 30,
        "a repeated minimum is also the penultimate minimum");

  // out = c + (((pmin + pmax - 2c) * (7 - cvg) + 4) >> 3). Worked from the
  // case above with coverage 3: (120 + 100 - 200) * 4 + 4 = 84, >> 3 = 10,
  // so 110.
  check(n64_video::vi_anti_alias_channel(100, three_neighbours, 3) == 110,
        "VI anti-aliasing blends toward the reconstructed background");
  // Darker background: centre 200, neighbours 20, 10, 30, coverage 1. Values
  // 200 200 20 10 30: pmax 200, pmin 20; (20 + 200 - 400) * 6 + 4 = -1076,
  // floor(-1076 / 8) = -135, so 65.
  const int darker[] = {20, 10, 30};
  check(n64_video::vi_anti_alias_channel(200, darker, 1) == 65,
        "VI anti-aliasing rounds a negative offset down");
  check(n64_video::vi_anti_alias_channel(200, darker, 7) == 200,
        "full coverage leaves the centre unchanged");
  check(n64_video::vi_anti_alias_channel(77, no_neighbours, 0) == 77,
        "without fully covered neighbours the pixel stays");

  // Divot: per channel the median of the pixel and its left and right
  // neighbours, when any of the three is partially covered.
  check(n64_video::divot_median(10, 50, 30) == 30, "divot median of a spike");
  check(n64_video::divot_median(50, 10, 30) == 30, "divot median of a dip");
  check(n64_video::divot_median(5, 5, 9) == 5, "divot median with a tie");
  check(n64_video::divot_applies(7, 6, 7) && n64_video::divot_applies(5, 7, 7) &&
            n64_video::divot_applies(7, 7, 0),
        "the divot filter applies when any of the three pixels is an edge");
  check(!n64_video::divot_applies(7, 7, 7), "the divot filter skips fully covered runs");

  // RDP sample positions (angrylion coverage.c, parallel-RDP coverage.h): 4
  // sub-scanlines, 2 samples each, at x 0 and 4/8 on rows 0 and 2, 2/8 and
  // 6/8 on rows 1 and 3. RT64 draws RDP point (x, y) at the rendered pixel
  // centre (x + 0.5, y + 0.5), so RDP pixel (0, 0)'s sample at (k/8, j/8) sits
  // 2k/16, 2j/16 from rendered pixel (0, 0)'s centre; an offset of 8/16 or
  // more falls into the next rendered pixel, 16/16 back.
  using n64_video::MsaaSamplePosition;
  const auto pattern_8x = n64_video::msaa_sample_pattern(8);
  const MsaaSamplePosition expected_8x[] = {
      {0, 0, 0, 0},   {-8, 0, 1, 0},  {4, 4, 0, 0},   {-4, 4, 1, 0},
      {0, -8, 0, 1},  {-8, -8, 1, 1}, {4, -4, 0, 1},  {-4, -4, 1, 1}};
  check(pattern_8x.count == 8 && std::equal(std::begin(expected_8x), std::end(expected_8x),
                                            pattern_8x.positions.begin()),
        "8x MSAA takes the RDP's 8 sample positions, sample 0 at the top left");
  // 4x keeps one sample per sub-scanline: (0,0), (6/8,2/8), (4/8,4/8), (2/8,6/8).
  const auto pattern_4x = n64_video::msaa_sample_pattern(4);
  const MsaaSamplePosition expected_4x[] = {
      {0, 0, 0, 0}, {-4, 4, 1, 0}, {-8, -8, 1, 1}, {4, -4, 0, 1}};
  check(pattern_4x.count == 4 && std::equal(std::begin(expected_4x), std::end(expected_4x),
                                            pattern_4x.positions.begin()),
        "4x MSAA keeps one RDP sample per sub-scanline");
  check(n64_video::msaa_sample_pattern(2).count == 0,
        "other sample counts have no RDP pattern");
  // Bit 2s: sample s lies in the next rendered pixel to the right; bit 2s+1:
  // in the one below. For 8x: samples 1, 3, 5, 7 right; 4-7 below: 0xEE44.
  check(n64_video::sample_pixel_offset_bits(pattern_8x) == 0xEE44U,
        "sample pixel offsets pack two bits per sample");

  // Per-sample render-mode record: what the RDP would do with coverage for
  // the draw that wrote the sample.
  using n64_video::CoverageDestination;
  using n64_video::DrawCoverageMode;
  using n64_video::SampleRecord;
  const DrawCoverageMode aa_opaque{CoverageDestination::clamp, true, false};
  const DrawCoverageMode aa_cut_out{CoverageDestination::clamp, true, true};
  const DrawCoverageMode opaque{CoverageDestination::full, false, false};
  const DrawCoverageMode opaque_clamp{CoverageDestination::clamp, false, false};
  const DrawCoverageMode cloud{CoverageDestination::save, false, false};
  const DrawCoverageMode aa_translucent{CoverageDestination::wrap, true, false};
  check(n64_video::sample_record(aa_opaque, 1.0f) == SampleRecord{true, 8},
        "an anti-aliased draw records partial coverage");
  check(n64_video::sample_record(aa_cut_out, 0.5f) == SampleRecord{true, 4},
        "coverage times alpha scales the sample's coverage");
  check(n64_video::sample_record(aa_cut_out, 0.0f) == SampleRecord{true, 0},
        "coverage times alpha with alpha 0 contributes nothing");
  check(n64_video::sample_record(opaque, 1.0f) == SampleRecord{false, 8},
        "CVG_DST_FULL forces full coverage");
  check(n64_video::sample_record({CoverageDestination::full, true, true}, 0.25f) ==
            SampleRecord{false, 8},
        "CVG_DST_FULL forces full coverage even with AA and alpha");
  check(n64_video::sample_record(opaque_clamp, 1.0f) == SampleRecord{false, 8},
        "without AA_EN a clamped draw writes full coverage");
  check(!n64_video::sample_record(cloud, 1.0f).has_value(),
        "CVG_DST_SAVE keeps the coverage under it");
  check(!n64_video::sample_record(aa_translucent, 1.0f).has_value(),
        "CVG_DST_WRAP keeps the coverage under it (documented deviation)");
  check(n64_video::sample_record({CoverageDestination::clamp, true, false, true}, 1.0f) ==
            SampleRecord{false, 8},
        "copy and fill draws write full coverage");

  // The record rides in the colour target's alpha above RT64's own coverage
  // value, whose low 3 bits RT64 writes back to RDRAM: value = rt64 + 8 * code.
  for (const SampleRecord record : {SampleRecord{true, 8}, SampleRecord{false, 0},
                                    SampleRecord{true, 3}, SampleRecord{false, 8}}) {
    const unsigned value = n64_video::encode_sample_record(7, record);
    check((value & 7U) == 7 && n64_video::decode_sample_record(value) == record &&
              value <= n64_video::sample_record_max_value,
          "a record round-trips and keeps RT64's low coverage bits");
  }
  check(n64_video::decode_sample_record(7) == SampleRecord{false, 8},
        "alpha without a record (RDRAM load, clear) reads as full coverage");
  check(n64_video::decode_sample_record(255) == SampleRecord{false, 8},
        "an out-of-range alpha reads as full coverage");

  // Front surface: samples within 8 * dz of the front-most one, dz the local
  // depth slope from the neighbouring pixels (the smaller difference per axis,
  // so an edge on one side doesn't widen it).
  const float plane_tolerance = n64_video::front_depth_tolerance({0.5f, 0.49f, 0.51f, 0.48f, 0.52f});
  check(plane_tolerance > 8 * 0.03f - 1e-4f && plane_tolerance < 8 * 0.03f + 1e-3f,
        "tolerance is 8 times the summed depth slope");
  const float edge_tolerance = n64_video::front_depth_tolerance({0.5f, 0.49f, 0.9f, 0.5f, 0.5f});
  check(edge_tolerance < 8 * 0.011f, "a depth jump on one side doesn't widen the tolerance");
  check(n64_video::front_depth_tolerance({0.5f, 0.5f, 0.5f, 0.5f, 0.5f}) > 0.0f,
        "a flat surface keeps a minimum tolerance");
  const float silhouette_depths[] = {0.2f, 0.9f, 0.2f, 0.9f, 0.2f, 0.9f, 0.9f, 0.9f};
  check(n64_video::front_surface_mask(silhouette_depths, 0.01f) == 0x15U,
        "the front surface is the nearest samples within the tolerance");
  const float seam_depths[] = {0.5f, 0.501f, 0.502f, 0.503f, 0.5f, 0.5f, 0.501f, 0.502f};
  check(n64_video::front_surface_mask(seam_depths, 0.01f) == 0xFFU,
        "an inner edge of one surface keeps every sample in front");

  // Pixel coverage from the front samples' records.
  const unsigned own_8x = n64_video::own_pixel_mask(pattern_8x);
  check(own_8x == 0x05U, "samples 0 and 2 lie in the RDP pixel's own rendered pixel");
  std::array<SampleRecord, 8> records{};
  records.fill(SampleRecord{true, 8});
  auto silhouette = n64_video::pixel_coverage(records, pattern_8x, 0x15U);
  check(silhouette.coverage == 2 && silhouette.colour_mask == 0x05U,
        "an anti-aliased silhouette over 3 of 8 samples stores coverage 2 and "
        "takes its colour from the front samples in its own pixel");
  auto interior = n64_video::pixel_coverage(records, pattern_8x, 0xFFU);
  check(interior.coverage == 7, "an anti-aliased interior is fully covered");
  auto right_side = n64_video::pixel_coverage(records, pattern_8x, 0xAAU);
  check(right_side.coverage == 3 && right_side.colour_mask == 0xAAU,
        "front samples only in neighbouring pixels give the colour");
  records.fill(SampleRecord{false, 8});
  auto sharp = n64_video::pixel_coverage(records, pattern_8x, 0x15U);
  check(sharp.coverage == 7 && sharp.colour_mask == 0x01U,
        "a non-anti-aliased front is full and point-sampled at the top left");
  records.fill(SampleRecord{true, 4});
  check(n64_video::pixel_coverage(records, pattern_8x, 0xFFU).coverage == 3,
        "alpha-scaled samples sum to the pixel's coverage");
  records.fill(SampleRecord{true, 0});
  check(n64_video::pixel_coverage(records, pattern_8x, 0xFFU).coverage == 0,
        "no coverage stores the least coverage");
  records.fill(SampleRecord{false, 4});
  check(n64_video::pixel_coverage(records, pattern_8x, 0xFFU).coverage == 3,
        "a non-anti-aliased cut-out takes the top-left sample's alpha coverage");
  std::array<SampleRecord, 8> records_4x{};
  records_4x.fill(SampleRecord{true, 8});
  check(n64_video::pixel_coverage(records_4x, pattern_4x, 0x3U).coverage == 3,
        "4x counts each sample as two eighths");

  // Where coverage can't be estimated, the frame falls back to the averaged
  // colour at full coverage.
  using n64_video::CoverageUnavailable;
  check(n64_video::coverage_unavailable({8, true, 1.0f, 1.0f, 1}) == CoverageUnavailable::none,
        "8x MSAA at native resolution estimates coverage");
  check(n64_video::coverage_unavailable({4, true, 1.0f, 1.0f, 1}) == CoverageUnavailable::none,
        "4x MSAA estimates coverage");
  check(n64_video::coverage_unavailable({1, false, 1.0f, 1.0f, 1}) == CoverageUnavailable::no_msaa,
        "without MSAA there's no coverage");
  check(n64_video::coverage_unavailable({8, true, 2.0f, 2.0f, 1}) ==
            CoverageUnavailable::scaled_resolution,
        "a resolution scale other than 1 isn't estimated");
  check(n64_video::coverage_unavailable({8, true, 1.33f, 1.0f, 1}) ==
            CoverageUnavailable::scaled_resolution,
        "an aspect scale other than 1 isn't estimated");
  check(n64_video::coverage_unavailable({8, true, 1.0f, 1.0f, 2}) ==
            CoverageUnavailable::downsampled,
        "downsampling isn't estimated");

  std::cout << "N64 video coverage checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
