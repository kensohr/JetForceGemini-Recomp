#pragma once
// Screen curvature checkbox of the native video settings dialog. It only
// applies while a CRT filter is selected under Original video output;
// otherwise it is disabled and keeps its value.
#include "../../include/jfg/renderer/crt_curvature_settings.hpp"
#include "video_settings_resource.h"
#include <windows.h>

namespace jfg::frontend {

inline void init_crt_curvature_control(HWND window, bool on) {
  CheckDlgButton(window, IDC_VIDEO_CRT_CURVATURE, on ? BST_CHECKED : BST_UNCHECKED);
}

inline bool selected_crt_curvature(HWND window) {
  return IsDlgButtonChecked(window, IDC_VIDEO_CRT_CURVATURE) == BST_CHECKED;
}

inline void enable_crt_curvature_control(HWND window, VideoOutput output, CrtPreset crt_filter) {
  EnableWindow(GetDlgItem(window, IDC_VIDEO_CRT_CURVATURE), crt_curvature_available(output, crt_filter));
}

} // namespace jfg::frontend
