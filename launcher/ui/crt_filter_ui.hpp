#pragma once
// CRT filter card of the RmlUi video settings page (Display tab): one segment
// button per entry of jfg::crt_presets, ids video-crt-<ui_id>. The setting
// only applies under Original video output; under Enhanced the buttons are
// disabled and keep showing the stored preset.
#include "../../include/jfg/renderer/crt_filter_settings.hpp"
#include <RmlUi/Core.h>
#include <string>
#include <string_view>

namespace jfg_video {

inline constexpr const char *crt_filter_search_keywords =
    "crt filter scanlines rgb monitor pvm tv aperture grille mask display retro";

inline constexpr std::string_view crt_filter_id_prefix = "video-crt-";

inline std::string crt_filter_button_id(const jfg::CrtPresetChoice &choice) {
  return std::string(crt_filter_id_prefix) + std::string(choice.ui_id);
}

inline void refresh_crt_filter(Rml::ElementDocument *doc, const jfg::VideoSettings &draft) {
  const bool available = jfg::crt_filter_available(draft.output);
  for (const auto &choice : jfg::crt_presets) {
    auto *button = doc->GetElementById(crt_filter_button_id(choice));
    const bool on = draft.crt_filter == choice.preset;
    button->SetClass("selected", on);
    button->SetAttribute("aria-checked", on ? "true" : "false");
    // RmlUi's :disabled only matches form controls, not these buttons, so
    // the grey-out is set here (the page's disabled look: opacity 0.55).
    if (available) {
      button->RemoveAttribute("disabled");
      button->RemoveProperty("opacity");
    } else {
      button->SetAttribute("disabled", true);
      button->SetProperty("opacity", "0.55");
    }
  }
  doc->GetElementById("video-crt-hint")->SetInnerRML(
      !available ? "Needs Original video output."
      : draft.crt_filter == jfg::CrtPreset::off
          ? "Shows the native picture without simulating a display."
          : "Simulates the display the console was connected to: scanlines and a phosphor mask.");
}

// Applies a click on one of the card's buttons to `draft`. Returns false when
// `id` isn't one of them. Disabled buttons never reach here.
inline bool choose_crt_filter(const std::string &id, jfg::VideoSettings &draft) {
  for (const auto &choice : jfg::crt_presets) {
    if (id == crt_filter_button_id(choice)) {
      draft.crt_filter = choice.preset;
      return true;
    }
  }
  return false;
}

} // namespace jfg_video
