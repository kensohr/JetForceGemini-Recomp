#pragma once
#include <optional>
#include <string_view>

// The Screen curvature setting's video.ini values (see GLOSSARY "Screen
// curvature"): crt_curvature=0 or 1. An unknown value reads as off;
// video_settings_warnings (video_settings_warnings.hpp) reports it.
namespace jfg {

inline constexpr std::string_view crt_curvature_ini_key = "crt_curvature";

inline constexpr std::string_view crt_curvature_ini_value(bool on) noexcept {
  return on ? "1" : "0";
}

// The value's meaning, or nullopt when it isn't one the setting writes.
inline constexpr std::optional<bool> parse_crt_curvature(std::string_view value) noexcept {
  if (value == crt_curvature_ini_value(true))
    return true;
  if (value == crt_curvature_ini_value(false))
    return false;
  return std::nullopt;
}

} // namespace jfg
