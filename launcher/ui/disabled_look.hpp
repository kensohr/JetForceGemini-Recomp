#pragma once
// Disabled state of a launcher card button that isn't a form control. RmlUi's
// :disabled only matches form controls, so the page's disabled look
// (video-settings.rcss: opacity 0.55) is set on these buttons here.
#include <RmlUi/Core.h>

namespace jfg_video {

// Keep in step with the :disabled opacity in video-settings.rcss.
inline constexpr const char *disabled_button_opacity = "0.55";

inline void set_button_enabled(Rml::Element *button, bool enabled) {
  if (enabled) {
    button->RemoveAttribute("disabled");
    button->RemoveProperty("opacity");
  } else {
    button->SetAttribute("disabled", true);
    button->SetProperty("opacity", disabled_button_opacity);
  }
}

} // namespace jfg_video
