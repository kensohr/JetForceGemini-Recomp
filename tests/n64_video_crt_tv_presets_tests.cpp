// Video core CRT filter, TV presets: the S-Video TV and Composite TV CRT
// presets' luma/chroma blur, YIQ transform and slot mask in the arithmetic
// the CRT shader include shares with C++ (n64_video/crt_filter.h), and the
// project glue that maps JFG's setting onto them. Kept apart from
// n64_video_crt_filter_tests.cpp, whose RGB monitor checks stay as they were.
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

bool near(const crt::CrtTriple &a, const crt::CrtTriple &b, float tolerance) {
  return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

void check_yiq_round_trip() {
  const crt::CrtTriple colors[] = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f},
                                   {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.8f, 0.3f, 0.55f}};
  for (const auto &rgb : colors) {
    const auto yiq = crt::CrtRgbToYiq(rgb);
    check(near(crt::CrtRgbToYiq(crt::CrtYiqToRgb(yiq)), yiq, 1e-4f), "YIQ -> RGB -> YIQ is the identity");
    check(near(crt::CrtYiqToRgb(yiq), rgb, 1e-4f), "RGB -> YIQ -> RGB is the identity");
  }
  // NTSC luma weights; grey carries no chroma.
  check(near(crt::CrtRgbToYiq({1.0f, 0.0f, 0.0f}), {0.299f, 0.596f, 0.211f}, 2e-3f), "red in YIQ");
  check(near(crt::CrtRgbToYiq({0.5f, 0.5f, 0.5f}), {0.5f, 0.0f, 0.0f}, 1e-4f), "grey has no chroma");
}

// The kernel's centre of mass, in source pixels (positive: to the right).
double kernel_centre(const float (&kernel)[crt::CrtSignalKernelTaps]) {
  double centre = 0.0;
  for (unsigned tap = 0; tap < crt::CrtSignalKernelTaps; ++tap)
    centre += kernel[tap] * (double(tap) - double(crt::CrtSignalKernelRadius));
  return centre;
}

double kernel_sum(const float (&kernel)[crt::CrtSignalKernelTaps]) {
  double sum = 0.0;
  for (const float weight : kernel)
    sum += weight;
  return sum;
}

void check_kernels() {
  for (unsigned preset = crt::CrtPresetRgbMonitor; preset < crt::CrtPresetCount; ++preset) {
    const auto kernels = crt::CrtMakeSignalKernels(preset);
    check(std::fabs(kernel_sum(kernels.luma) - 1.0) < 1e-5, "luma kernel sums to 1");
    check(std::fabs(kernel_sum(kernels.chroma) - 1.0) < 1e-5, "chroma kernel sums to 1");
  }
  // RGB monitor's kernels pass every source pixel through unchanged, so its
  // picture stays as it was before the TV presets.
  const auto rgb = crt::CrtMakeSignalKernels(crt::CrtPresetRgbMonitor);
  check(rgb.luma[crt::CrtSignalKernelRadius] == 1.0f && rgb.chroma[crt::CrtSignalKernelRadius] == 1.0f,
        "RGB monitor doesn't blur");

  const auto &composite = crt::CrtPresets[crt::CrtPresetCompositeTv];
  const auto &svideo = crt::CrtPresets[crt::CrtPresetSvideoTv];
  const auto &monitor = crt::CrtPresets[crt::CrtPresetRgbMonitor];
  check(composite.lumaWidth > svideo.lumaWidth && svideo.lumaWidth >= monitor.lumaWidth,
        "luma widths order as Composite > S-Video >= RGB monitor");
  check(composite.chromaWidth > svideo.chromaWidth && svideo.chromaWidth >= monitor.chromaWidth,
        "chroma widths order as Composite > S-Video >= RGB monitor");
  check(monitor.lumaWidth == monitor.chromaWidth && monitor.chromaShift == 0.0f,
        "RGB monitor has equal widths and no shift");
  check(svideo.chromaWidth >= 2.0f && svideo.chromaWidth <= 3.0f && svideo.lumaWidth < svideo.chromaWidth,
        "S-Video: slight luma blur, moderate chroma blur");
  check(std::fabs(kernel_centre(crt::CrtMakeSignalKernels(crt::CrtPresetSvideoTv).chroma)) < 1e-5,
        "S-Video doesn't shift chroma");
  check(composite.chromaShift > 0.0f && composite.chromaShift < 0.5f,
        "Composite shifts chroma right by well under a source pixel");
}

constexpr int window_centre = int(crt::CrtSignalWindowTaps / 2);

// The blurred signal `position` source pixels right of a red/black edge: red
// on the left of the edge, black from it on.
crt::CrtTriple behind_red_edge(int position, unsigned preset) {
  crt::CrtTriple window[crt::CrtSignalWindowTaps];
  for (int tap = 0; tap < int(crt::CrtSignalWindowTaps); ++tap) {
    const bool red = position + tap - window_centre < 0;
    window[tap] = crt::CrtMakeTriple(red ? 1.0f : 0.0f, 0.0f, 0.0f);
  }
  return crt::CrtBlurSignal(window, window_centre, crt::CrtMakeSignalKernels(preset));
}

// How much chroma a red/black edge spreads into the black: the chroma
// magnitude summed over the source pixels right of the edge.
double chroma_past_red_edge(unsigned preset) {
  double chroma = 0.0;
  for (int position = 0; position < 8; ++position) {
    const auto yiq = crt::CrtRgbToYiq(behind_red_edge(position, preset));
    chroma += std::sqrt(double(yiq.y) * yiq.y + double(yiq.z) * yiq.z);
  }
  return chroma;
}

void check_signal_blur() {
  for (unsigned preset = crt::CrtPresetRgbMonitor; preset < crt::CrtPresetCount; ++preset) {
    const auto kernels = crt::CrtMakeSignalKernels(preset);
    for (const auto &color : {crt::CrtMakeTriple(1.0f, 1.0f, 1.0f), crt::CrtMakeTriple(0.7f, 0.2f, 0.4f)}) {
      crt::CrtTriple flat[crt::CrtSignalWindowTaps];
      for (auto &pixel : flat)
        pixel = color;
      for (int centre = int(crt::CrtSignalKernelRadius);
           centre < int(crt::CrtSignalWindowTaps) - int(crt::CrtSignalKernelRadius); ++centre)
        check(near(crt::CrtBlurSignal(flat, centre, kernels), color, 1e-5f), "a flat row keeps its colour");
    }
  }
  {
    // RGB monitor passes the signal through.
    crt::CrtTriple row[crt::CrtSignalWindowTaps];
    for (int tap = 0; tap < int(crt::CrtSignalWindowTaps); ++tap)
      row[tap] = crt::CrtMakeTriple(float(tap % 3) * 0.5f, float(tap % 5) * 0.25f, float(tap % 2));
    const auto kernels = crt::CrtMakeSignalKernels(crt::CrtPresetRgbMonitor);
    bool unchanged = true;
    for (int centre = int(crt::CrtSignalKernelRadius);
         centre < int(crt::CrtSignalWindowTaps) - int(crt::CrtSignalKernelRadius); ++centre)
      unchanged = unchanged && near(crt::CrtBlurSignal(row, centre, kernels), row[centre], 1e-5f);
    check(unchanged, "RGB monitor passes the signal through");
  }
  const double composite = chroma_past_red_edge(crt::CrtPresetCompositeTv);
  const double svideo = chroma_past_red_edge(crt::CrtPresetSvideoTv);
  const double monitor = chroma_past_red_edge(crt::CrtPresetRgbMonitor);
  check(composite > svideo * 1.3 && svideo > monitor, "a red/black edge spreads chroma further on Composite than on S-Video");
  check(monitor < 1e-6, "RGB monitor spreads no chroma");
  {
    const auto pixel = behind_red_edge(0, crt::CrtPresetCompositeTv);
    check(pixel.x >= 0.0f && pixel.x <= 1.0f && pixel.y >= 0.0f && pixel.y <= 1.0f && pixel.z >= 0.0f &&
              pixel.z <= 1.0f,
          "the blurred signal stays in the signal range");
  }
}

// How much darker a flat field is between two scanlines than on one.
double scanline_dip(unsigned preset) {
  auto beam = [preset](float distance) {
    return crt::CrtScanlineWeight(distance + 1.0f, preset) + crt::CrtScanlineWeight(distance, preset) +
           crt::CrtScanlineWeight(distance - 1.0f, preset);
  };
  return 1.0 - beam(0.5f) / beam(0.0f);
}

constexpr unsigned tv_presets[] = {crt::CrtPresetCompositeTv, crt::CrtPresetSvideoTv};

void check_slot_mask() {
  for (const unsigned preset : tv_presets) {
    check(crt::CrtPresets[preset].maskType == crt::CrtMaskSlot, "TV presets use a slot mask");
    bool triads = true, gaps = true, offset = true;
    for (unsigned column = 0; column < 12; ++column) {
      unsigned gap_rows = 0, gap_row = 0;
      for (unsigned row = 0; row < 4; ++row) {
        unsigned lit = 0;
        for (unsigned channel = 0; channel < 3; ++channel) {
          const float mask = crt::CrtMaskAt(column, row, channel, preset);
          if (mask > 1.0f) {
            ++lit;
            triads = triads && channel == column % 3;
          }
        }
        if (lit == 0) {
          ++gap_rows;
          gap_row = row;
        }
      }
      // One dark row per 4-row period; alternate triads' gaps 2 rows apart.
      gaps = gaps && gap_rows == 1;
      const unsigned expected_gap = (column / 3) % 2 == 0 ? 3 : 1;
      offset = offset && gap_row == expected_gap;
      for (unsigned row = 0; row < 4; ++row)
        for (unsigned channel = 0; channel < 3; ++channel)
          offset = offset && crt::CrtMaskAt(column, row, channel, preset) ==
                                 crt::CrtMaskAt(column, row + 4, channel, preset);
    }
    check(triads, "slot mask: one RGB triad per three output columns");
    check(gaps, "slot mask: one dark row in every four");
    check(offset, "slot mask: alternate triads offset by 2 rows, 4-row period");
    const auto &tv = crt::CrtPresets[preset];
    const auto &monitor = crt::CrtPresets[crt::CrtPresetRgbMonitor];
    check(tv.maskLight / tv.maskDark < monitor.maskLight / monitor.maskDark, "slot mask is weaker than the grille");
    check(scanline_dip(preset) < 0.25 && scanline_dip(crt::CrtPresetRgbMonitor) > 2.0 * scanline_dip(preset),
          "TV scanlines are faint, much softer than RGB monitor's");
  }
  check(crt::CrtPresets[crt::CrtPresetCompositeTv].scanlineHardness ==
            crt::CrtPresets[crt::CrtPresetSvideoTv].scanlineHardness,
        "both TV presets share their scanline constants");
}

// Mean linear output of full white per output row, over two mask triads,
// through scanlines, boost and mask, for a picture `columns` wide.
std::vector<double> white_rows(unsigned output_height, unsigned preset, unsigned first_column, unsigned columns) {
  constexpr unsigned scanlines = 240;
  std::vector<double> rows;
  for (unsigned y = 0; y < output_height; ++y) {
    const float position = crt::CrtScanlinePosition(float(y) + 0.5f, float(output_height), scanlines);
    const float distance = position - (std::floor(position) + 0.5f);
    double total = 0.0;
    for (unsigned column = first_column; column < first_column + columns; ++column)
      for (unsigned channel = 0; channel < 3; ++channel)
        total += crt::CrtChannelAt(1.0f, 1.0f, 1.0f, distance, column, y, channel, preset);
    rows.push_back(total / (columns * 3));
  }
  return rows;
}

double mean(const std::vector<double> &values) {
  double total = 0.0;
  for (const double value : values)
    total += value;
  return total / double(values.size());
}

std::vector<double> box_filter(const std::vector<double> &values, unsigned width) {
  std::vector<double> filtered;
  for (std::size_t first = 0; first + width <= values.size(); ++first) {
    double total = 0.0;
    for (unsigned i = 0; i < width; ++i)
      total += values[first + i];
    filtered.push_back(total / width);
  }
  return filtered;
}

// Moire: brightness variation slower than both the scanlines and the mask.
// Box filters one scanline period (`scanline_rows`, whole rows) and one mask
// period wide remove both; what is left is their beat.
double beat(const std::vector<double> &rows, unsigned scanline_rows) {
  const auto smooth = box_filter(box_filter(rows, scanline_rows), 4);
  const auto [low, high] = std::minmax_element(smooth.begin() + 16, smooth.end() - 16);
  return (*high - *low) / mean(rows);
}

void check_white_and_moire() {
  for (unsigned preset = crt::CrtPresetRgbMonitor; preset < crt::CrtPresetCount; ++preset) {
    for (const unsigned height : {1080u, 1440u}) {
      const double white = mean(white_rows(height, preset, 0, 6));
      check(white >= 0.70 && white <= 1.0, "every preset keeps at least 70% of full white");
    }
  }
  // 1080 rows are 4.5 per scanline (9 rows hold two), 1440 rows are 6.
  for (const unsigned preset : tv_presets) {
    check(beat(white_rows(1080, preset, 0, 6), 9) < 0.01, "slot mask: no moire with the scanlines at 1080p");
    check(beat(white_rows(1440, preset, 0, 6), 6) < 0.01, "slot mask: no moire with the scanlines at 1440p");
    check(beat(white_rows(1080, preset, 0, 3), 9) < 0.02 && beat(white_rows(1440, preset, 0, 3), 6) < 0.02,
          "slot mask: one triad column barely beats with the scanlines");
  }
}

} // namespace

int main() {
  check_yiq_round_trip();
  check_kernels();
  check_signal_blur();
  check_slot_mask();
  check_white_and_moire();
  check(jfg::video_core_crt_preset(jfg::CrtPreset::composite_tv) == n64_video::CrtPreset::composite_tv,
        "glue maps Composite TV");
  check(jfg::video_core_crt_preset(jfg::CrtPreset::svideo_tv) == n64_video::CrtPreset::svideo_tv,
        "glue maps S-Video TV");
  {
    n64_video::VideoOptions options;
    options.original_video_output = true;
    options.crt_preset = n64_video::CrtPreset::composite_tv;
    check(n64_video::crt_shader_preset(options) == crt::CrtPresetCompositeTv, "Composite TV shader preset id");
    options.crt_preset = n64_video::CrtPreset::svideo_tv;
    check(n64_video::crt_shader_preset(options) == crt::CrtPresetSvideoTv, "S-Video TV shader preset id");
  }

  std::cout << "CRT TV preset checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
