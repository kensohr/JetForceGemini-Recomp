#pragma once
// Screen curvature card of the RmlUi video settings page (Display tab): one
// switch, id video-crt-curvature. It only applies while a CRT filter is
// selected under Original video output; otherwise the switch is disabled and
// keeps showing the stored value.
#include "../../include/jfg/renderer/crt_curvature_settings.hpp"
#include <RmlUi/Core.h>
#include <string>

namespace jfg_video {

inline constexpr const char *crt_curvature_search_keywords =
    "screen curvature curved crt tube bend vignette corners display retro";

inline constexpr const char *crt_curvature_switch_id = "video-crt-curvature";

inline void refresh_crt_curvature(Rml::ElementDocument *doc, const jfg::VideoSettings &draft) {
  const bool available = jfg::crt_curvature_available(draft);
  auto *button = doc->GetElementById(crt_curvature_switch_id);
  button->SetClass("selected", draft.crt_curvature);
  button->SetAttribute("aria-checked", draft.crt_curvature ? "true" : "false");
  // RmlUi's :disabled only matches form controls, so the grey-out is set
  // here, as the CRT filter card does.
  if (available) {
    button->RemoveAttribute("disabled");
    button->RemoveProperty("opacity");
  } else {
    button->SetAttribute("disabled", true);
    button->SetProperty("opacity", "0.55");
  }
  doc->GetElementById("video-crt-curvature-hint")->SetInnerRML(
      available ? "Bends the picture like a CRT tube, with darker, rounded corners."
                : "Needs a CRT filter and Original video output.");
}

// Applies a click on the card's switch to `draft`. Returns false when `id`
// isn't the switch. Disabled switches never reach here.
inline bool choose_crt_curvature(const std::string &id, jfg::VideoSettings &draft) {
  if (id != crt_curvature_switch_id)
    return false;
  draft.crt_curvature = !draft.crt_curvature;
  return true;
}

} // namespace jfg_video
