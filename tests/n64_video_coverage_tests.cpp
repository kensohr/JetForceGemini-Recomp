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
#include <string>

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

  // Per-sample render-mode record: an anti-aliased draw marks the samples it
  // covers in the colour target's alpha, next to RT64's own coverage value.
  using n64_video::CoverageDestination;
  using n64_video::DrawCoverageMode;
  using n64_video::SampleRecord;
  const DrawCoverageMode aa_opaque{CoverageDestination::clamp, true, false};
  const DrawCoverageMode aa_cut_out{CoverageDestination::clamp, true, true};
  const DrawCoverageMode opaque{CoverageDestination::full, false, false};
  const DrawCoverageMode opaque_clamp{CoverageDestination::clamp, false, false};
  const DrawCoverageMode cloud{CoverageDestination::save, false, false};
  const DrawCoverageMode aa_translucent{CoverageDestination::wrap, true, false};
  const DrawCoverageMode aa_translucent_alpha{CoverageDestination::wrap, true, true};
  const DrawCoverageMode fill{CoverageDestination::clamp, true, false, true};
  check(n64_video::records_partial(aa_opaque) && n64_video::records_partial(aa_cut_out),
        "an anti-aliased clamped draw records partial coverage");
  check(!n64_video::records_partial(opaque) && !n64_video::records_partial(opaque_clamp) &&
            !n64_video::records_partial({CoverageDestination::full, true, true}),
        "CVG_DST_FULL and draws without AA_EN record a full pixel");
  check(!n64_video::records_partial(fill), "copy and fill draws record a full pixel");
  check(!n64_video::records_partial(cloud) && !n64_video::records_partial(aa_translucent),
        "CVG_DST_SAVE and CVG_DST_WRAP add nothing to what lies under them");

  // RT64's alpha arithmetic, per sample, in units of the alpha (1/255): a
  // CVG_DST_SAVE or CVG_DST_WRAP draw adds its shader output to the stored
  // value, any other draw replaces it. `output` is what RT64's raster shader
  // writes: 7 for a full sample, round(8 * alpha) under CVG_X_ALPHA (clamped
  // to 7 for CVG_DST_CLAMP), 0 for SAVE.
  constexpr unsigned alpha_range = 255;
  auto blend = [](unsigned stored, const DrawCoverageMode &mode, unsigned output) {
    const bool additive = (mode.destination == CoverageDestination::save) ||
                          (mode.destination == CoverageDestination::wrap);
    return std::min(additive ? stored + output : output, alpha_range);
  };
  struct Draw {
    DrawCoverageMode mode;
    unsigned output;
  };
  // Under `msaa` the sample holds RT64's value alone; under `coverage` the
  // raster shader adds the record. The value RT64 reads back (resolve,
  // RDRAM, framebuffer textures) must be the same.
  auto msaa_and_coverage = [&](std::initializer_list<Draw> draws) {
    unsigned msaa = 0;
    unsigned coverage = 0;
    for (const Draw &draw : draws) {
      msaa = blend(msaa, draw.mode, draw.output);
      coverage = blend(coverage, draw.mode, n64_video::record_sample_alpha(draw.output, draw.mode));
    }
    return std::array<unsigned, 2>{msaa, n64_video::rt64_coverage_value(coverage, alpha_range)};
  };
  auto rt64_value_kept = [&](std::initializer_list<Draw> draws) {
    const auto values = msaa_and_coverage(draws);
    return values[0] == values[1];
  };
  check(rt64_value_kept({{aa_opaque, 7}}) && rt64_value_kept({{aa_cut_out, 3}}) &&
            rt64_value_kept({{opaque, 7}}) && rt64_value_kept({{fill, 7}}),
        "RT64's coverage value reads back unchanged under the record");
  check(rt64_value_kept({{aa_opaque, 7}, {aa_translucent, 8}}) &&
            rt64_value_kept({{aa_opaque, 7}, {aa_translucent_alpha, 4}, {aa_translucent_alpha, 5}}),
        "CVG_DST_WRAP adds RT64's coverage on top of an anti-aliased sample as under msaa");
  check(rt64_value_kept({{opaque, 7}, {aa_translucent_alpha, 3}}) &&
            rt64_value_kept({{aa_translucent, 8}}),
        "CVG_DST_WRAP over a full or empty sample matches msaa");
  check(rt64_value_kept({{aa_cut_out, 2}, {cloud, 0}}),
        "CVG_DST_SAVE leaves RT64's value as under msaa");
  // RT64's resolve averages the samples' values and writes bit 2 of the
  // rounded result back as the 16-bit pixel's coverage bit. An edge pixel
  // half covered by an anti-aliased draw over a full surface: per sample the
  // values read back match, so the averaged bit does too.
  {
    unsigned msaa_sum = 0;
    unsigned coverage_sum = 0;
    for (unsigned s = 0; s < 8; s++) {
      const Draw edge = (s < 4) ? Draw{aa_opaque, 7} : Draw{opaque, 7};
      const auto values = msaa_and_coverage({{opaque, 7}, edge, {aa_translucent_alpha, s}});
      msaa_sum += values[0];
      coverage_sum += values[1];
    }
    check(msaa_sum == coverage_sum, "an averaged edge pixel resolves as under msaa");
  }
  check(n64_video::rt64_coverage_value(alpha_range, alpha_range) == alpha_range &&
            n64_video::rt64_coverage_value(65535, 65535) == 65535,
        "alpha loaded from RDRAM or cleared to full stays as it is");

  // What the estimate reads from a sample: whether its front draw
  // anti-aliased, and the eighths of the sample it covered (RT64's value; 7,
  // RT64's full sample, counts as all 8).
  auto record_of = [&](std::initializer_list<Draw> draws) {
    unsigned coverage = 0;
    for (const Draw &draw : draws)
      coverage = blend(coverage, draw.mode, n64_video::record_sample_alpha(draw.output, draw.mode));
    return n64_video::decode_sample_record(coverage, alpha_range);
  };
  check(record_of({{aa_opaque, 7}}) == SampleRecord{true, 8},
        "an anti-aliased sample reads as partial and fully covered");
  check(record_of({{aa_cut_out, 4}}) == SampleRecord{true, 4},
        "coverage times alpha scales the sample's coverage");
  check(record_of({{opaque, 7}}) == SampleRecord{false, 8} &&
            record_of({{opaque_clamp, 7}}) == SampleRecord{false, 8},
        "a full or non-anti-aliased sample reads as a full pixel");
  check(record_of({{opaque_clamp, 3}}) == SampleRecord{false, 3},
        "a non-anti-aliased cut-out keeps its alpha coverage");
  check(record_of({{aa_cut_out, 3}, {cloud, 0}}) == SampleRecord{true, 3},
        "CVG_DST_SAVE keeps the record under it");
  check(record_of({{aa_cut_out, 3}, {aa_translucent, 8}}) == SampleRecord{true, 8},
        "CVG_DST_WRAP adds its coverage, clamped to the full sample (documented deviation)");
  check(record_of({}) == SampleRecord{false, 8} &&
            n64_video::decode_sample_record(alpha_range, alpha_range) == SampleRecord{false, 8},
        "alpha without a draw (clear, RDRAM load) reads as a full pixel");

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
  using n64_video::CoverageStatus;
  check(n64_video::coverage_status({8, true, 1.0f, 1.0f, 1}) == CoverageStatus::estimating,
        "8x MSAA at native resolution estimates coverage");
  check(n64_video::coverage_status({4, true, 1.0f, 1.0f, 1}) == CoverageStatus::estimating,
        "4x MSAA estimates coverage");
  check(n64_video::coverage_status({1, false, 1.0f, 1.0f, 1}) == CoverageStatus::no_msaa,
        "without MSAA there's no coverage");
  check(n64_video::coverage_status({8, true, 2.0f, 2.0f, 1}) == CoverageStatus::scaled_resolution,
        "a resolution scale other than 1 isn't estimated");
  check(n64_video::coverage_status({8, true, 1.33f, 1.0f, 1}) == CoverageStatus::scaled_resolution,
        "an aspect scale other than 1 isn't estimated");
  check(n64_video::coverage_status({8, true, 1.0f, 1.0f, 2}) == CoverageStatus::downsampled,
        "downsampling isn't estimated");

  // Each status is reported once: the estimate at debug level, each fallback
  // as a warning, with the values that caused it.
  using n64_video::CoverageLogLevel;
  check(n64_video::coverage_status_log_level(CoverageStatus::estimating) == CoverageLogLevel::debug,
        "a running estimate is logged at debug level");
  check(n64_video::coverage_status_log_level(CoverageStatus::no_msaa) == CoverageLogLevel::warning &&
            n64_video::coverage_status_log_level(CoverageStatus::scaled_resolution) ==
                CoverageLogLevel::warning &&
            n64_video::coverage_status_log_level(CoverageStatus::downsampled) ==
                CoverageLogLevel::warning &&
            n64_video::coverage_status_log_level(CoverageStatus::no_depth) == CoverageLogLevel::warning,
        "fallbacks are logged as warnings");
  auto mentions = [](const std::string &message, const char *value) {
    return message.find(value) != std::string::npos;
  };
  check(mentions(n64_video::coverage_status_message(CoverageStatus::estimating, {8, true, 1.0f, 1.0f, 1}),
                 "8x"),
        "the estimate's message names the sample count");
  check(mentions(n64_video::coverage_status_message(CoverageStatus::no_msaa, {2, true, 1.0f, 1.0f, 1}),
                 "2x"),
        "the no-MSAA message names the sample count");
  check(mentions(n64_video::coverage_status_message(CoverageStatus::scaled_resolution,
                                                    {8, true, 1.33f, 2.0f, 1}),
                 "1.33 x 2.00"),
        "the scaled-resolution message names the scale");
  check(mentions(n64_video::coverage_status_message(CoverageStatus::downsampled, {8, true, 1.0f, 1.0f, 3}),
                 "3"),
        "the downsampling message names the multiplier");
  std::cout << "N64 video coverage checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
