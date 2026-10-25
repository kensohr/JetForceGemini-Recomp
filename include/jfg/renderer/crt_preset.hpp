#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The CRT filter setting's values (see GLOSSARY "CRT filter", "CRT preset"):
// Off or one CRT preset. Each value is listed once in crt_presets, in the
// order the launcher shows them; a new preset adds an enum value and a row.
namespace jfg {

enum class CrtPreset : std::uint32_t { off, rgb_monitor };

struct CrtPresetChoice {
  CrtPreset preset;
  // The value written to video.ini under crt_filter.
  std::string_view ini_value;
  // The launcher's label.
  std::string_view label;
  // The launcher's control id suffix (video-crt-<id>).
  std::string_view ui_id;
};

inline constexpr std::array<CrtPresetChoice, 2> crt_presets{{
    {CrtPreset::off, "off", "Off", "off"},
    {CrtPreset::rgb_monitor, "rgb_monitor", "RGB monitor", "rgb-monitor"},
}};

inline constexpr bool valid_crt_preset(CrtPreset preset) noexcept {
  for (const auto &choice : crt_presets)
    if (choice.preset == preset)
      return true;
  return false;
}

inline constexpr std::string_view crt_preset_ini_value(CrtPreset preset) noexcept {
  for (const auto &choice : crt_presets)
    if (choice.preset == preset)
      return choice.ini_value;
  return crt_presets.front().ini_value;
}

inline constexpr std::string_view crt_preset_label(CrtPreset preset) noexcept {
  for (const auto &choice : crt_presets)
    if (choice.preset == preset)
      return choice.label;
  return crt_presets.front().label;
}

inline constexpr std::optional<CrtPreset> parse_crt_preset(std::string_view value) noexcept {
  for (const auto &choice : crt_presets)
    if (choice.ini_value == value)
      return choice.preset;
  return std::nullopt;
}

} // namespace jfg
