// Video core checks for the VI's gamma-dither noise.
#include "n64_video/gamma_dither.hpp"
#include "n64_video/vi_filtering.hpp"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main() {
  unsigned failures = 0;
  auto check = [&](bool ok, const char *name) {
    if (!ok) {
      ++failures;
      std::cerr << "FAIL: " << name << '\n';
    }
  };

  // VI registers as libultra's NTSC low-res mode sets them (see
  // n64_video_tests.cpp).
  constexpr std::uint32_t vi_h_start_ntsc = 0x006C02ECU;
  constexpr std::uint32_t vi_x_scale_320 = 0x00000200U;
  auto filtering = [&](std::uint32_t control) {
    return n64_video::vi_filtering({control, vi_h_start_ntsc, vi_x_scale_320});
  };

  // VI_CONTROL bit 2 is gamma dither, bit 3 gamma. JFG's 0x13016 has gamma
  // dither on and gamma off.
  using n64_video::GammaDither;
  check(filtering(0x13016U).gamma_dither == GammaDither::noise,
        "gamma dither without gamma adds noise");
  check(filtering(0x13012U).gamma_dither == GammaDither::off,
        "no noise with the gamma-dither bit clear");
  check(filtering(0x1301EU).gamma_dither == GammaDither::unsupported_with_gamma,
        "gamma dither with gamma is reported as unsupported");
  check(n64_video::ViFiltering{}.gamma_dither == GammaDither::off,
        "default filtering adds no noise");

  // The noise bit adds to each 8-bit channel; a channel at 255 stays there
  // (angrylion vi_process_full: no gamma, dither adds 0 or 1 below 255).
  check(n64_video::gamma_dither_channel(100, 0) == 100, "a 0 bit leaves a mid value");
  check(n64_video::gamma_dither_channel(100, 1) == 101, "a 1 bit raises a mid value by one");
  check(n64_video::gamma_dither_channel(0, 1) == 1, "a 1 bit raises black by one");
  check(n64_video::gamma_dither_channel(254, 1) == 255, "a 1 bit raises 254 to 255");
  check(n64_video::gamma_dither_channel(255, 1) == 255, "255 stays 255");
  check(n64_video::gamma_dither_channel(255, 0) == 255, "255 stays 255 with a 0 bit");

  // The noise bits of a 32x32 grid of output samples: bit 0 red, 1 green,
  // 2 blue. Fair noise sets each bit in about half the samples, and two
  // samples or channels agree about half the time; the bounds leave room for
  // chance (1024 samples: one standard deviation is 16).
  constexpr std::uint32_t grid_size = 32;
  constexpr unsigned sample_count = grid_size * grid_size;
  constexpr unsigned near_half_low = sample_count * 2 / 5;
  constexpr unsigned near_half_high = sample_count * 3 / 5;
  auto near_half = [&](unsigned count) {
    return count >= near_half_low && count <= near_half_high;
  };
  auto count_samples = [&](auto predicate) {
    unsigned count = 0;
    for (std::uint32_t y = 0; y < grid_size; ++y)
      for (std::uint32_t x = 0; x < grid_size; ++x)
        count += predicate(x, y) ? 1U : 0U;
    return count;
  };
  auto bit = [](std::uint32_t x, std::uint32_t y, std::uint32_t frame, unsigned channel) {
    return (n64_video::gamma_dither_bits(x, y, frame) >> channel) & 1U;
  };
  constexpr std::uint32_t frame = 7;
  const char *channel_names[] = {"red", "green", "blue"};
  for (unsigned channel = 0; channel < 3; ++channel) {
    const std::string name = channel_names[channel];
    check(near_half(count_samples([&](auto x, auto y) { return bit(x, y, frame, channel) == 1; })),
          (name + " noise is 1 in about half the samples").c_str());
    check(near_half(count_samples(
              [&](auto x, auto y) { return bit(x, y, frame, channel) == bit(x + 1, y, frame, channel); })),
          (name + " noise shows no vertical stripes").c_str());
    check(near_half(count_samples(
              [&](auto x, auto y) { return bit(x, y, frame, channel) == bit(x, y + 1, frame, channel); })),
          (name + " noise shows no horizontal stripes").c_str());
    check(near_half(count_samples(
              [&](auto x, auto y) { return bit(x, y, frame, channel) == bit(x, y, frame + 1, channel); })),
          (name + " noise changes with the frame counter").c_str());
    const unsigned next_channel = (channel + 1) % 3;
    check(near_half(count_samples(
              [&](auto x, auto y) { return bit(x, y, frame, channel) == bit(x, y, frame, next_channel); })),
          (name + " noise is independent of the next channel").c_str());
  }
  check((n64_video::gamma_dither_bits(3, 5, frame) & ~0x7U) == 0, "only three noise bits");
  check(n64_video::gamma_dither_bits(3, 5, frame) == n64_video::gamma_dither_bits(3, 5, frame),
        "the noise is a function of position and frame");

  // Gamma with gamma dither is warned about once per run, naming VI_CONTROL;
  // the supported cases aren't reported.
  static std::vector<std::string> warnings;
  n64_video::gamma_dither_log_sink() = [](const char *message) { warnings.emplace_back(message); };
  n64_video::report_gamma_dither(0x13016U);
  n64_video::report_gamma_dither(0x13012U);
  check(warnings.empty(), "supported gamma-dither settings aren't reported");
  n64_video::report_gamma_dither(0x1301EU);
  n64_video::report_gamma_dither(0x1301EU);
  n64_video::report_gamma_dither(0x1311EU);
  check(warnings.size() == 1, "gamma with gamma dither is reported once per run");
  check(!warnings.empty() && warnings.front().find("0x1301E") != std::string::npos,
        "the warning names the VI_CONTROL value");
  n64_video::gamma_dither_log_sink() = nullptr;

  std::cout << "N64 video gamma dither checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
