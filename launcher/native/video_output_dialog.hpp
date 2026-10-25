#pragma once
// Video output drop-down of the native video settings dialog: Enhanced, then
// Original (the order video_settings_dialog.cpp adds them in).
#include "../../include/jfg/renderer/video_settings.hpp"
#include "video_settings_resource.h"
#include <windows.h>

namespace jfg::frontend {

inline constexpr LRESULT video_output_original_index = 1;

inline VideoOutput selected_video_output(HWND window) {
  const auto index = SendDlgItemMessageW(window, IDC_VIDEO_OUTPUT, CB_GETCURSEL, 0, 0);
  return index == video_output_original_index ? VideoOutput::original : VideoOutput::enhanced;
}

} // namespace jfg::frontend
