#pragma once
#include "crt_curvature.hpp"
#include "crt_filter_settings.hpp"
#include "video_settings.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// When the Screen curvature setting applies, and the video.ini values that
// read as a default instead of making the file corrupt. Curvature bends the
// CRT filter's picture, so the launcher only offers it while a CRT filter is
// selected under Original video output; otherwise the stored value is kept
// but ignored.
namespace jfg {

inline constexpr bool crt_curvature_available(const VideoSettings &settings) noexcept {
  return effective_crt_preset(settings) != CrtPreset::off;
}

// Whether the runtime bends the picture for these settings.
inline constexpr bool effective_crt_curvature(const VideoSettings &settings) noexcept {
  return crt_curvature_available(settings) && settings.crt_curvature;
}

// One message per line of `text` (a video.ini) whose value parse_video_settings
// reads as the default because it doesn't know it (crt_curvature reads as off,
// crt_filter as Off). The messages leave out the value: video.log carries no
// text taken from files.
inline std::vector<std::string> video_settings_warnings(std::string_view text) {
  std::vector<std::string> warnings;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    const auto equal = line.find('=');
    if (equal == std::string_view::npos)
      continue;
    const auto key = line.substr(0, equal), value = line.substr(equal + 1);
    if (key == crt_curvature_ini_key && !parse_crt_curvature(value))
      warnings.push_back("video.ini " + std::string(crt_curvature_ini_key) +
                         " is not 0 or 1; screen curvature reads as off");
    if (key == crt_filter_ini_key && !parse_crt_preset(value))
      warnings.push_back("video.ini " + std::string(crt_filter_ini_key) +
                         " is not a known CRT preset; the CRT filter reads as Off");
  }
  return warnings;
}

// video_settings_warnings for the file at `path`; none when it can't be read
// (load_video_settings reports that).
inline std::vector<std::string> load_video_settings_warnings(const std::filesystem::path &path) {
  if (path.empty())
    return {};
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return video_settings_warnings(text);
}

} // namespace jfg
