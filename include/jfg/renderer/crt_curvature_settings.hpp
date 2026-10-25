#pragma once
#include "crt_filter_settings.hpp"
#include "video_settings.hpp"

// When the Screen curvature setting applies. Curvature bends the CRT filter's
// picture, so the launcher only offers it while a CRT filter is selected
// under Original video output; otherwise the stored value is kept but
// ignored.
namespace jfg {

inline constexpr bool crt_curvature_available(VideoOutput output, CrtPreset crt_filter) noexcept {
  return crt_filter_available(output) && crt_filter != CrtPreset::off;
}

inline constexpr bool crt_curvature_available(const VideoSettings &settings) noexcept {
  return crt_curvature_available(settings.output, settings.crt_filter);
}

// Whether the runtime bends the picture for these settings.
inline constexpr bool effective_crt_curvature(const VideoSettings &settings) noexcept {
  return crt_curvature_available(settings) && settings.crt_curvature;
}

} // namespace jfg
