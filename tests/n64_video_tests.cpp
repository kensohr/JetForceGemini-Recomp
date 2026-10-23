#include "n64_video/rt64_configuration.hpp"
#include "n64_video/colour_reduction.hpp"
#include "n64_video/video_options.hpp"
#include <cstdint>
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

  // VI_CONTROL as a game sets it for a 16-bit framebuffer: divot on, dither
  // filter on, AA mode 0, colour type 2. Colour type 3 is a 32-bit framebuffer.
  constexpr std::uint32_t vi_control_16_bit = 0x13016U;
  constexpr std::uint32_t vi_control_32_bit = 0x13017U;
  constexpr std::uint32_t vi_control_blank = 0x13014U;
  const n64_video::VideoOptions original_options{true};
  const n64_video::VideoOptions enhanced_options{};

  const auto reduced = n64_video::colour_reduction(original_options, vi_control_16_bit);
  check(reduced.enabled, "original reduces a 16-bit framebuffer");
  check(reduced.pattern == n64_video::DitherPattern::magic_square,
        "original dithers with the magic square pattern");
  check(!n64_video::colour_reduction(original_options, vi_control_32_bit).enabled,
        "original leaves a 32-bit framebuffer unreduced");
  check(!n64_video::colour_reduction(original_options, vi_control_blank).enabled,
        "original leaves a blank VI unreduced");
  check(!n64_video::colour_reduction(enhanced_options, vi_control_16_bit).enabled,
        "enhanced never reduces");

  Rt64UserConfiguration applied;
  n64_video::apply_to_rt64(original_options, applied);
  check(n64_video::active_options().original_video_output,
        "applying original makes it active for the VI pass");
  n64_video::apply_to_rt64(enhanced_options, applied);
  check(!n64_video::active_options().original_video_output,
        "applying enhanced makes it active for the VI pass");

  std::cout << "N64 video core checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
