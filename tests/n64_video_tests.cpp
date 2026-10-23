#include "n64_video/rt64_configuration.hpp"
#include "n64_video/colour_reduction.hpp"
#include "n64_video/video_options.hpp"
#include "n64_video/vi_filtering.hpp"
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

  // VI registers as libultra's NTSC low-res mode sets them: active scanline
  // from 108 to 748 (640 samples), X scale 0x200 (half a framebuffer pixel
  // per sample, 2.10 fixed point), X offset 0.
  constexpr std::uint32_t vi_h_start_ntsc = 0x006C02ECU;
  constexpr std::uint32_t vi_x_scale_320 = 0x00000200U;
  const auto game_filtering =
      n64_video::vi_filtering({vi_control_16_bit, vi_h_start_ntsc, vi_x_scale_320});
  check(game_filtering.dither_filter, "VI_CONTROL 0x13016 enables the dither filter");
  check(game_filtering.resampling == n64_video::Resampling::interpolate,
        "AA mode 0 interpolates between framebuffer pixels");
  check(game_filtering.active_width == 640, "active width is hEnd - hStart");
  check(game_filtering.x_step == 0x200, "X step is VI_X_SCALE's scale field");
  check(game_filtering.x_offset == 0, "X offset is VI_X_SCALE's offset field");

  constexpr std::uint32_t vi_control_no_dither_filter = 0x03016U;
  check(!n64_video::vi_filtering({vi_control_no_dither_filter, vi_h_start_ntsc, vi_x_scale_320})
             .dither_filter,
        "dither filter stays off when its VI_CONTROL bit is clear");

  // AA mode is VI_CONTROL bits 9:8; mode 3 turns resampling's interpolation
  // off and replicates pixels.
  constexpr std::uint32_t vi_control_aa_mode_3 = 0x13316U;
  check(n64_video::vi_filtering({vi_control_aa_mode_3, vi_h_start_ntsc, vi_x_scale_320})
                .resampling == n64_video::Resampling::replicate,
        "AA mode 3 replicates the nearest framebuffer pixel");
  constexpr std::uint32_t vi_control_aa_mode_1 = 0x13116U;
  constexpr std::uint32_t vi_control_aa_mode_2 = 0x13216U;
  check(n64_video::vi_filtering({vi_control_aa_mode_1, vi_h_start_ntsc, vi_x_scale_320})
                    .resampling == n64_video::Resampling::interpolate &&
            n64_video::vi_filtering({vi_control_aa_mode_2, vi_h_start_ntsc, vi_x_scale_320})
                    .resampling == n64_video::Resampling::interpolate,
        "AA modes 1 and 2 interpolate");

  // A hi-res mode: 640 framebuffer pixels over 640 samples, offset 0.25.
  constexpr std::uint32_t vi_x_scale_640_offset = 0x01000400U;
  const auto hi_res =
      n64_video::vi_filtering({vi_control_16_bit, vi_h_start_ntsc, vi_x_scale_640_offset});
  check(hi_res.x_step == 0x400 && hi_res.x_offset == 0x100,
        "X step and offset follow VI_X_SCALE");

  constexpr std::uint32_t vi_h_start_off = 0x02EC006CU;
  check(n64_video::vi_filtering({vi_control_16_bit, vi_h_start_off, vi_x_scale_320})
                .active_width == 0,
        "no active scanline when hEnd is before hStart");

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
