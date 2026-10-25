#pragma once
// CRT filter control of the native video settings dialog: a drop-down with
// one entry per jfg::crt_presets row, in that order. It only applies under
// Original video output; under Enhanced it is disabled and keeps its value.
#include "../../include/jfg/renderer/crt_filter_settings.hpp"
#include "video_settings_resource.h"
#include <string>
#include <windows.h>

namespace jfg::frontend {

inline void init_crt_filter_control(HWND window, CrtPreset preset) {
  int selected = 0;
  for (std::size_t i = 0; i < crt_presets.size(); ++i) {
    const auto &choice = crt_presets[i];
    // The labels are ASCII, so widening each character is exact.
    const std::wstring label(choice.label.begin(), choice.label.end());
    SendDlgItemMessageW(window, IDC_VIDEO_CRT_FILTER, CB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM>(label.c_str()));
    if (choice.preset == preset)
      selected = static_cast<int>(i);
  }
  SendDlgItemMessageW(window, IDC_VIDEO_CRT_FILTER, CB_SETCURSEL,
                      static_cast<WPARAM>(selected), 0);
}

inline CrtPreset selected_crt_filter(HWND window) {
  const auto index = SendDlgItemMessageW(window, IDC_VIDEO_CRT_FILTER, CB_GETCURSEL, 0, 0);
  const bool listed = index >= 0 && static_cast<std::size_t>(index) < crt_presets.size();
  return listed ? crt_presets[static_cast<std::size_t>(index)].preset : CrtPreset::off;
}

inline void enable_crt_filter_control(HWND window, VideoOutput output) {
  EnableWindow(GetDlgItem(window, IDC_VIDEO_CRT_FILTER), crt_filter_available(output));
}

} // namespace jfg::frontend
