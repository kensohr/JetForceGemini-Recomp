#include "n64_video/rt64_configuration.hpp"
#include "n64_video/video_options.hpp"
#include <iostream>

namespace {
// Mirrors the members of RT64::UserConfiguration the core may touch, with
// RT64's defaults, so the core is checked without linking RT64.
struct Rt64UserConfiguration {
  enum class Resolution { Original, WindowIntegerScale, Manual, OptionCount };
  enum class Filtering { Nearest, Linear, AntiAliasedPixelScaling, OptionCount };
  enum class AspectRatio { Original, Expand, Manual, OptionCount };
  Resolution resolution = Resolution::WindowIntegerScale;
  double resolutionMultiplier = 2.0;
  int downsampleMultiplier = 1;
  Filtering filtering = Filtering::AntiAliasedPixelScaling;
  AspectRatio aspectRatio = AspectRatio::Original;
  bool operator==(const Rt64UserConfiguration &) const = default;
};
} // namespace

int main() {
  unsigned failures = 0;
  auto check = [&](bool ok, const char *name) {
    if (!ok) {
      ++failures;
      std::cerr << "FAIL: " << name << '\n';
    }
  };
  check(!n64_video::VideoOptions{}.original_video_output,
        "default options keep enhanced output");

  const Rt64UserConfiguration rt64_defaults;
  Rt64UserConfiguration enhanced;
  n64_video::apply_to_rt64(n64_video::VideoOptions{}, enhanced);
  check(enhanced == rt64_defaults, "enhanced leaves RT64 configuration unchanged");

  Rt64UserConfiguration original;
  n64_video::apply_to_rt64(n64_video::VideoOptions{true}, original);
  check(original.resolution == Rt64UserConfiguration::Resolution::Original,
        "original renders at native resolution");
  check(original.filtering == Rt64UserConfiguration::Filtering::AntiAliasedPixelScaling,
        "original keeps RT64's pixel-scaling filter");
  check(original.aspectRatio == rt64_defaults.aspectRatio &&
            original.downsampleMultiplier == rt64_defaults.downsampleMultiplier,
        "original keeps RT64's window fit");

  std::cout << "N64 video core checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
