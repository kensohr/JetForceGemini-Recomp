// Video output control of the native video settings dialog. Kept apart from
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
      L"JfgVideoOutputTest-" + std::to_wstring(GetCurrentProcessId());
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
  HWND owner = CreateWindowExW(0, L"STATIC", L"Video output test owner",
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
  auto apply = [&]() { SendMessageW(window, WM_COMMAND, IDC_VIDEO_APPLY, 0); };
  auto selected_output = [&]() {
    return SendDlgItemMessageW(window, IDC_VIDEO_OUTPUT, CB_GETCURSEL, 0, 0);
  };
  apply();
  check(selected_output() == 0 && applies == 1 &&
            accepted.rendering.output == jfg::VideoOutput::enhanced,
        "default video output is Enhanced");
  choose(IDC_VIDEO_OUTPUT, 1);
  apply();
  check(applies == 2 && accepted.rendering.output == jfg::VideoOutput::original,
        "Apply returns Original video output");
  SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
  window = jfg::frontend::create_video_settings(owner, model);
  check(selected_output() == 1, "reopen retains applied video output");
  SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
  DestroyWindow(owner);
  std::cout << "Native video output dialog checks: " << failures
            << " failures\n";
  return failures ? 1 : 0;
}
