// Video core screen curvature: the warp, picture fit, vignette and rounded
// corners the CRT shader include shares with C++ (n64_video/crt_filter.h),
// and when the VI pass passes the curvature flag to the CRT shader. Kept
// apart from n64_video_crt_filter_tests.cpp so ticket-sized changes to either
// stay independent.
#include "n64_video/crt_filter.hpp"
#include <cmath>
#include <iostream>

namespace crt = n64_video::crt;

namespace {

unsigned failures = 0;

void check(bool ok, const char *name) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}

constexpr float width = 1440.0f;
constexpr float height = 1080.0f;
// Positions are compared in output pixels.
constexpr float tolerance = 1e-3f;

bool near(float a, float b) { return std::fabs(a - b) <= tolerance; }

float curved_x(float x, float y, unsigned preset, bool curvature = true) {
  return crt::CrtCurvedX(x, y, width, height, preset, curvature);
}

float curved_y(float x, float y, unsigned preset, bool curvature = true) {
  return crt::CrtCurvedY(x, y, width, height, preset, curvature);
}

// The output position the warp maps onto picture position (x, y): the warp
// is close to the identity, so stepping by the residual converges.
void output_position_of(float x, float y, unsigned preset, float &output_x, float &output_y) {
  output_x = x;
  output_y = y;
  for (int step = 0; step < 100; ++step) {
    output_x += x - curved_x(output_x, output_y, preset);
    output_y += y - curved_y(output_x, output_y, preset);
  }
}

// A grid of output positions across the rectangle, edges included.
constexpr int grid_steps = 16;
float grid_x(int i) { return width * float(i) / float(grid_steps); }
float grid_y(int j) { return height * float(j) / float(grid_steps); }

void check_curvature_off_is_identity() {
  bool identity = true;
  for (unsigned preset = 1; preset < crt::CrtPresetCount; ++preset)
    for (int i = 0; i <= grid_steps; ++i)
      for (int j = 0; j <= grid_steps; ++j)
        identity = identity && curved_x(grid_x(i), grid_y(j), preset, false) == grid_x(i) &&
                   curved_y(grid_x(i), grid_y(j), preset, false) == grid_y(j);
  check(identity, "curvature off leaves every position exactly where it is");
}

void check_shape(unsigned preset) {
  check(near(curved_x(width / 2, height / 2, preset), width / 2) &&
            near(curved_y(width / 2, height / 2, preset), height / 2),
        "the centre is fixed");

  bool symmetric = true, monotonic = true;
  for (int i = 0; i <= grid_steps; ++i) {
    for (int j = 0; j <= grid_steps; ++j) {
      const float x = grid_x(i), y = grid_y(j);
      const float mirrored_x = width - x, mirrored_y = height - y;
      symmetric = symmetric && near(curved_x(mirrored_x, y, preset), width - curved_x(x, y, preset)) &&
                  near(curved_x(x, mirrored_y, preset), curved_x(x, y, preset)) &&
                  near(curved_y(x, mirrored_y, preset), height - curved_y(x, y, preset)) &&
                  near(curved_y(mirrored_x, y, preset), curved_y(x, y, preset));
      if (i > 0)
        monotonic = monotonic && curved_x(x, y, preset) > curved_x(grid_x(i - 1), y, preset);
      if (j > 0)
        monotonic = monotonic && curved_y(x, y, preset) > curved_y(x, grid_y(j - 1), preset);
    }
  }
  check(symmetric, "the warp is symmetric about both axes");
  check(monotonic, "the warp is monotonic along each axis");
}

void check_fit(unsigned preset) {
  check(near(curved_x(0.0f, height / 2, preset), 0.0f) && near(curved_x(width, height / 2, preset), width) &&
            near(curved_y(width / 2, 0.0f, preset), 0.0f) && near(curved_y(width / 2, height, preset), height),
        "Fit: edge midpoints land on the rectangle edge");

  bool inside = true, pulled_in = true;
  for (const float corner_x : {0.0f, width}) {
    for (const float corner_y : {0.0f, height}) {
      float x = 0.0f, y = 0.0f;
      output_position_of(corner_x, corner_y, preset, x, y);
      inside = inside && x >= -tolerance && x <= width + tolerance && y >= -tolerance && y <= height + tolerance;
      // Curved horizontally, the corner moves in from the left or right edge.
      if (crt::CrtPresets[preset].curvatureX > 0.0f)
        pulled_in = pulled_in && std::fabs(x - corner_x) > 1.0f;
      if (crt::CrtPresets[preset].curvatureY > 0.0f)
        pulled_in = pulled_in && std::fabs(y - corner_y) > 1.0f;
    }
  }
  check(inside, "Fit: every source corner lands inside the rectangle");
  check(pulled_in, "Fit: corners pull inward along each curved axis");
}

void check_rgb_monitor() {
  check(crt::CrtPresets[crt::CrtPresetRgbMonitor].curvatureX > 0.0f, "RGB monitor curves horizontally");
  bool unchanged = true;
  for (int i = 0; i <= grid_steps; ++i)
    for (int j = 0; j <= grid_steps; ++j)
      unchanged = unchanged && near(curved_y(grid_x(i), grid_y(j), crt::CrtPresetRgbMonitor), grid_y(j));
  check(unchanged, "RGB monitor's y = 0 leaves vertical positions unchanged");
}

void check_vignette() {
  check(crt::CrtVignette(width / 2, height / 2, width, height) == 1.0f, "the vignette is 1 at the centre");
  // Along the diagonal and along each axis towards the edges and corners.
  bool falls_off = true;
  float diagonal = 1.0f, across = 1.0f, down = 1.0f;
  for (int step = 1; step <= grid_steps / 2; ++step) {
    const float dx = width / 2 * float(step) / float(grid_steps / 2);
    const float dy = height / 2 * float(step) / float(grid_steps / 2);
    const float next_diagonal = crt::CrtVignette(width / 2 + dx, height / 2 + dy, width, height);
    const float next_across = crt::CrtVignette(width / 2 + dx, height / 2, width, height);
    const float next_down = crt::CrtVignette(width / 2, height / 2 - dy, width, height);
    falls_off = falls_off && next_diagonal < diagonal && next_across < across && next_down < down &&
                next_diagonal > 0.0f;
    diagonal = next_diagonal;
    across = next_across;
    down = next_down;
  }
  check(falls_off, "the vignette falls off monotonically towards the corners");
}

void check_rounded_corners() {
  check(crt::CrtPictureCoverage(width / 2, height / 2, width, height) == 1.0f, "the picture's centre is covered");
  check(crt::CrtPictureCoverage(-2.0f, height / 2, width, height) == 0.0f &&
            crt::CrtPictureCoverage(width / 2, height + 2.0f, width, height) == 0.0f,
        "outside the picture is black");
  const float edge = crt::CrtPictureCoverage(0.0f, height / 2, width, height);
  check(edge > 0.25f && edge < 0.75f, "the border is anti-aliased over about one output pixel");
  check(crt::CrtPictureCoverage(1.5f, height / 2, width, height) == 1.0f &&
            crt::CrtPictureCoverage(-1.5f, height / 2, width, height) == 0.0f,
        "the anti-aliased border is no wider than about one output pixel");
  check(crt::CrtPictureCoverage(2.0f, 2.0f, width, height) == 0.0f, "the corners are rounded off");
}

} // namespace

int main() {
  check_curvature_off_is_identity();
  for (unsigned preset = 1; preset < crt::CrtPresetCount; ++preset) {
    check_shape(preset);
    check_fit(preset);
  }
  check_rgb_monitor();
  check_vignette();
  check_rounded_corners();

  n64_video::VideoOptions options;
  options.original_video_output = true;
  options.crt_preset = n64_video::CrtPreset::rgb_monitor;
  options.crt_curvature = true;
  check(n64_video::crt_shader_curvature(options) == 1, "the CRT shader bends the picture when curvature is on");
  options.crt_preset = n64_video::CrtPreset::off;
  check(n64_video::crt_shader_curvature(options) == 0, "curvature is ignored without a CRT preset");
  options.crt_preset = n64_video::CrtPreset::rgb_monitor;
  options.original_video_output = false;
  check(n64_video::crt_shader_curvature(options) == 0, "curvature is ignored under Enhanced");
  options.original_video_output = true;
  options.crt_curvature = false;
  check(n64_video::crt_shader_curvature(options) == 0, "curvature off by default");

  std::cout << "CRT curvature checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
