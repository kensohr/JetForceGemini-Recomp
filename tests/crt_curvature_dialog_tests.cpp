// Screen curvature checkbox of the native video settings dialog: off by
// default, only enabled while a CRT filter is selected under Original video
// output, keeping its value while disabled. Kept apart from
// video_dialog_tests.cpp so upstream's test file stays unchanged.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../launcher/native/video_settings_dialog.hpp"
#include "../launcher/native/video_settings_resource.h"
#include <commctrl.h>
#include <iostream>
#include <string>
#include <windows.h>

int main() {
  // All UI is confined to an inactive desktop; never send desktop input or
  // steal focus from an owner's running game/application.
  const std::wstring desktop_name =
      L"JfgCrtCurvatureTest-" + std::to_wstring(GetCurrentProcessId());
  HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0,
                                 GENERIC_ALL, nullptr);
  if (!desktop || !SetThreadDesktop(desktop))
    return 2;
  unsigned failures = 0, applies = 0;
  auto check = [&](bool ok, const char *name) {
    if (!ok) {
      ++failures;
      std::cerr << "FAIL: " << name << '\n';
    }
  };
  HWND owner = CreateWindowExW(0, L"STATIC", L"Screen curvature test owner",
                               WS_OVERLAPPEDWINDOW, 0, 0, 900, 760, nullptr,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
  jfg::frontend::VideoDialogModel model;
  jfg::frontend::VideoDialogSettings accepted;
  model.apply = [&](const auto &settings, std::wstring &) {
    accepted = settings;
    ++applies;
    return true;
  };
  HWND window = jfg::frontend::create_video_settings(owner, model);
  if (!window)
    return 3;
  auto choose = [&](int id, int value) {
    SendDlgItemMessageW(window, id, CB_SETCURSEL, static_cast<WPARAM>(value),
                        0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(GetDlgItem(window, id)));
  };
  auto click_curvature = [&]() {
    const bool on = IsDlgButtonChecked(window, IDC_VIDEO_CRT_CURVATURE) == BST_CHECKED;
    CheckDlgButton(window, IDC_VIDEO_CRT_CURVATURE, on ? BST_UNCHECKED : BST_CHECKED);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_VIDEO_CRT_CURVATURE, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(window, IDC_VIDEO_CRT_CURVATURE)));
  };
  auto apply = [&]() { SendMessageW(window, WM_COMMAND, IDC_VIDEO_APPLY, 0); };
  auto checked = [&]() {
    return IsDlgButtonChecked(window, IDC_VIDEO_CRT_CURVATURE) == BST_CHECKED;
  };
  auto enabled = [&]() {
    return IsWindowEnabled(GetDlgItem(window, IDC_VIDEO_CRT_CURVATURE)) != FALSE;
  };
  check(GetDlgItem(window, IDC_VIDEO_CRT_CURVATURE) != nullptr, "the dialog has the checkbox");
  check(!checked() && !enabled(), "default is off, disabled under Enhanced");
  choose(IDC_VIDEO_OUTPUT, 1);
  check(!enabled(), "Original with the CRT filter off keeps it disabled");
  choose(IDC_VIDEO_CRT_FILTER, 1);
  check(enabled(), "a CRT filter under Original enables it");
  click_curvature();
  apply();
  check(applies == 1 && accepted.rendering.crt_curvature, "Apply returns curvature on");
  choose(IDC_VIDEO_CRT_FILTER, 0);
  check(!enabled() && checked(), "the CRT filter off disables it and keeps it on");
  choose(IDC_VIDEO_CRT_FILTER, 1);
  choose(IDC_VIDEO_OUTPUT, 0);
  check(!enabled() && checked(), "Enhanced disables it and keeps it on");
  apply();
  check(applies == 2 && accepted.rendering.crt_curvature &&
            accepted.rendering.output == jfg::VideoOutput::enhanced,
        "Apply keeps the disabled value");
  SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
  window = jfg::frontend::create_video_settings(owner, model);
  check(checked() && !enabled(), "reopen retains curvature, disabled");
  SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
  DestroyWindow(owner);
  std::cout << "Native screen curvature dialog checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
