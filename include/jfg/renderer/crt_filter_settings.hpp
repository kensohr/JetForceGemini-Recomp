#pragma once
#include "crt_preset.hpp"
#include "video_settings.hpp"

// When the CRT filter setting applies. It simulates a CRT on top of Original
// video output, so the launcher only offers it there; under Enhanced the
// stored preset is kept but ignored.
namespace jfg {

inline constexpr bool crt_filter_available(VideoOutput output) noexcept {
  return output == VideoOutput::original;
}

// The CRT preset the runtime shows for these settings.
inline constexpr CrtPreset effective_crt_preset(const VideoSettings &settings) noexcept {
  return crt_filter_available(settings.output) ? settings.crt_filter : CrtPreset::off;
}

} // namespace jfg
