// Video core CRT filter: the scanline, mask and brightness arithmetic the CRT
// shader include shares with C++ (n64_video/crt_filter.h), the scanline count
// per framebuffer height, when the filter runs, and the project glue that maps
// JFG's setting onto the core's preset.
#include "crt_filter_options.hpp"
#include "n64_video/crt_filter.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace crt = n64_video::crt;

namespace {

unsigned failures = 0;

void check(bool ok, const char *name) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}

// Each scanline's beam summed over the output rows of a picture `output_height`
// pixels tall, as the shader samples it at pixel centres.
std::vector<double> scanline_energies(unsigned output_height, unsigned scanlines, unsigned preset) {
  std::vector<double> energy(scanlines, 0.0);
  for (unsigned y = 0; y < output_height; ++y) {
    const float position = crt::CrtScanlinePosition(float(y) + 0.5f, float(output_height), scanlines);
    for (unsigned line = 0; line < scanlines; ++line) {
      const float distance = position - (float(line) + 0.5f);
      if (std::fabs(distance) <= crt::CrtScanlineReach)
        energy[line] += crt::CrtScanlineWeight(distance, preset);
    }
  }
  return energy;
}

void check_equal_scanline_energy(unsigned output_height, const char *name) {
  constexpr unsigned scanlines = 240;
  const auto energy = scanline_energies(output_height, scanlines, crt::CrtPresetRgbMonitor);
  // The first and last scanlines lose the part of their beam outside the
  // picture; every other one must carry the same light.
  const auto [low, high] = std::minmax_element(energy.begin() + 1, energy.end() - 1);
  const double spread = (*high - *low) / *high;
  check(*low > 0.0 && spread < 0.01, name);
}

// Mean linear output of full white over a block of output pixels, per channel
// of the three-column mask period, through scanlines, boost and mask.
double white_level(unsigned output_height, unsigned scanlines, unsigned preset) {
  double total = 0.0;
  unsigned samples = 0;
  for (unsigned y = 0; y < output_height; ++y) {
    const float position = crt::CrtScanlinePosition(float(y) + 0.5f, float(output_height), scanlines);
    const float distance = position - (std::floor(position) + 0.5f);
    for (unsigned column = 0; column < 6; ++column) {
      for (unsigned channel = 0; channel < 3; ++channel) {
        total += crt::CrtChannel(1.0f, 1.0f, 1.0f, distance, column, channel, preset);
        ++samples;
      }
    }
  }
  return total / samples;
}

} // namespace

int main() {
  check_equal_scanline_energy(1080, "every scanline carries the same light at 4.5x (1080 / 240)");
  check_equal_scanline_energy(1440, "every scanline carries the same light at 6x (1440 / 240)");

  {
    const double white = white_level(1080, 240, crt::CrtPresetRgbMonitor);
    check(white >= 0.70 && white <= 1.0, "RGB monitor keeps at least 70% of full white at 1080p");
    check(white_level(1440, 240, crt::CrtPresetRgbMonitor) >= 0.70,
          "RGB monitor keeps at least 70% of full white at 1440p");
  }
  check(crt::CrtChannel(0.0f, 0.0f, 0.0f, 0.0f, 0, 0, crt::CrtPresetRgbMonitor) == 0.0f, "black stays black");

  // Aperture grille: one RGB triad per three output columns.
  for (unsigned column = 0; column < 6; ++column) {
    for (unsigned channel = 0; channel < 3; ++channel) {
      const float mask = crt::CrtMask(column, channel, crt::CrtPresetRgbMonitor);
      const bool lit = (column % 3) == channel;
      check(lit ? mask > 1.0f : mask < 1.0f, "aperture grille lights one channel per column");
    }
  }

  check(crt::CrtScanlineWeight(0.0f, crt::CrtPresetRgbMonitor) == 1.0f, "a scanline is brightest at its centre");
  check(crt::CrtScanlineWeight(0.5f, crt::CrtPresetRgbMonitor) < 0.5f,
        "RGB monitor scanlines are pronounced: dark between lines");

  // Colour: decode with a CRT gamma of about 2.4, encode as sRGB.
  check(std::fabs(crt::CrtDecode(0.5f, crt::CrtPresetRgbMonitor) - std::pow(0.5f, 2.4f)) < 0.02f,
        "decode uses a CRT gamma of about 2.4");
  check(std::fabs(crt::CrtEncodeSrgb(0.0f)) < 1e-6f && std::fabs(crt::CrtEncodeSrgb(1.0f) - 1.0f) < 1e-5f &&
            std::fabs(crt::CrtEncodeSrgb(0.2140411f) - 0.5f) < 1e-3f,
        "sRGB encode");

  // One scanline per framebuffer row up to 288 rows, then one per row pair.
  check(crt::CrtScanlineCount(240) == 240 && crt::CrtScanlineCount(288) == 288, "progressive heights");
  check(crt::CrtScanlineCount(480) == 240 && crt::CrtScanlineCount(576) == 288, "interlaced heights");
  check(crt::CrtRowsPerScanline(240) == 1 && crt::CrtRowsPerScanline(480) == 2, "rows per scanline");

  {
    n64_video::VideoOptions options;
    options.crt_preset = n64_video::CrtPreset::rgb_monitor;
    check(!n64_video::crt_filter_active(options), "the CRT filter needs Original video output");
    options.original_video_output = true;
    check(n64_video::crt_filter_active(options), "RGB monitor runs under Original");
    check(n64_video::crt_shader_preset(options) == crt::CrtPresetRgbMonitor, "shader preset id");
    options.crt_preset = n64_video::CrtPreset::off;
    check(!n64_video::crt_filter_active(options) && n64_video::crt_shader_preset(options) == crt::CrtPresetOff,
          "Off leaves Original unfiltered");
    check(n64_video::VideoOptions{}.crt_preset == n64_video::CrtPreset::off, "default is Off");
  }

  check(jfg::video_core_crt_preset(jfg::CrtPreset::off) == n64_video::CrtPreset::off, "glue maps Off");
  check(jfg::video_core_crt_preset(jfg::CrtPreset::rgb_monitor) == n64_video::CrtPreset::rgb_monitor,
        "glue maps RGB monitor");

  std::cout << "CRT filter checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
