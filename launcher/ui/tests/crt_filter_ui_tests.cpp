// CRT filter card of the RmlUi video settings page: on the Display tab, only
// enabled under Original video output, keeping its value while greyed out,
// persisted and carried to the runtime. Kept apart from upstream's
// video_settings_ui_tests.cpp; the helpers below copy that file's, as
// video_output_ui_tests.cpp does.
// Usage: jfg_crt_filter_ui_tests.exe <new-artifact-directory> [--no-captures].
#include "../native_ui.cpp"
#include <stdexcept>
#include <tuple>
static fs::path home;
static unsigned checks = 0;
static bool capture_images = true;
static void demand(bool value, const char *why) {
  ++checks;
  if (!value)
    throw std::runtime_error(why);
}
static void settle() {
  for (unsigned i = 0; i < 3; ++i)
    FrontendUiFrame({});
}
static Rml::Element *element(const char *id) {
  auto *e = surface->video->el(id);
  demand(e != nullptr, id);
  return e;
}
// Clicks like a pointer would, first switching to the Display tab when the
// control is hidden. Clicking a disabled control is allowed: it must do
// nothing.
static void pointer(const char *id) {
  auto *e = element(id);
  if (!e->IsVisible(true)) {
    element("video-tab-display")->Click();
    settle();
  }
  e->ScrollIntoView();
  settle();
  auto point = e->GetAbsoluteOffset(Rml::BoxArea::Border) +
               Rml::Vector2f(e->GetClientWidth() / 2, e->GetClientHeight() / 2);
  auto *hit = surface->context->GetElementAtPoint(point);
  while (hit && hit != e)
    hit = hit->GetParentNode();
  demand(hit == e, "visible control receives pointer hits");
  surface->context->ProcessMouseMove(int(point.x), int(point.y), 0);
  surface->context->ProcessMouseButtonDown(0, 0);
  surface->context->ProcessMouseButtonUp(0, 0);
  settle();
}
static void capture(const char *name) {
  if (!capture_images) return;
  auto &s = *surface;
  settle();
  ComPtr<ID3D11Resource> resource;
  s.target->GetResource(&resource);
  ComPtr<ID3D11Texture2D> target;
  check(resource.As(&target));
  D3D11_TEXTURE2D_DESC desc{};
  target->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  check(s.device->CreateTexture2D(&desc, nullptr, &staging));
  s.gpu->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  check(s.gpu->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  std::ofstream file(home / name, std::ios::binary);
  unsigned char header[18]{};
  header[2] = 2;
  header[12] = static_cast<unsigned char>(desc.Width);
  header[13] = static_cast<unsigned char>(desc.Width >> 8);
  header[14] = static_cast<unsigned char>(desc.Height);
  header[15] = static_cast<unsigned char>(desc.Height >> 8);
  header[16] = 32;
  header[17] = 40;
  file.write(reinterpret_cast<char *>(header), 18);
  for (unsigned y = 0; y < desc.Height; ++y)
    file.write(static_cast<char *>(mapped.pData) + y * mapped.RowPitch,
               desc.Width * 4);
  s.gpu->Unmap(staging.Get(), 0);
}

static void check_crt_filter_protocol() {
  jfg::frontend::VideoRequest request;
  demand(request.crt_filter == static_cast<std::uint32_t>(jfg::CrtPreset::off),
         "default request carries CRT filter Off");
  request.version = 5;
  demand(!jfg::frontend::valid_video_request(request), "reject protocol without CRT filter");
  request = {};
  request.crt_filter = static_cast<std::uint32_t>(jfg::CrtPreset::rgb_monitor);
  demand(jfg::frontend::valid_video_request(request), "RGB monitor protocol valid");
  request.crt_filter = static_cast<std::uint32_t>(jfg::crt_presets.size());
  demand(!jfg::frontend::valid_video_request(request), "reject invalid CRT filter protocol");
}

static bool disabled(const char *id) { return element(id)->HasAttribute("disabled"); }

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  HWND window = nullptr;
  bool ready = false;
  try {
    int argc = 0;
    auto **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc != 2 && argc != 3) {
      LocalFree(argv);
      return 2;
    }
    home = argv[1];
    if (argc == 3) {
      if (std::wstring(argv[2]) != L"--no-captures") { LocalFree(argv); return 2; }
      capture_images = false;
    }
    LocalFree(argv);
    if (fs::exists(home))
      return 3;
    fs::create_directories(home);
    const auto desktopName =
        L"JfgRmlCrtFilterChecks-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0,
                                   GENERIC_ALL, nullptr);
    demand(desktop && SetThreadDesktop(desktop), "isolated inactive desktop");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = instance;
    cls.lpszClassName = L"JfgRmlCrtFilterTests";
    RegisterClassW(&cls);
    window = CreateWindowExW(0, cls.lpszClassName, L"Video Settings",
                             WS_OVERLAPPEDWINDOW, 40, 40, 720, 600, nullptr,
                             nullptr, instance, nullptr);
    const auto profile = home / "profile";
    fs::create_directories(profile);
    // A file written before the setting existed.
    std::ofstream(profile / "video.ini")
        << "version=1\nmode=custom\nfps=60\nvsync=1\nvideo_output=enhanced\n";
    demand(FrontendUiInit(
               window, profile, [](const std::string &) {}, "video"),
           "RmlUi initializes");
    ready = true;
    surface->video->save = [&](const jfg_video::Ui::Settings &value) {
      std::ofstream(profile / "video.ini")
          << jfg::serialize_video_settings(value.rendering);
      return true;
    };
    RECT frame{0, 0, 720, 600};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0,
                             GetDpiForWindow(window));
    SetWindowPos(window, nullptr, 40, 40, frame.right - frame.left,
                 frame.bottom - frame.top, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    FrontendUiResize();
    surface->context->SetDensityIndependentPixelRatio(1);
    settle();
    auto &ui = *surface->video;
    check_crt_filter_protocol();
    MSG close{};
    auto accepted = [&] {
      demand(PeekMessageW(&close, window, WM_CLOSE, WM_CLOSE, PM_REMOVE) != 0,
             "Apply accepts and closes");
    };
    pointer("video-tab-display");
    demand(element("video-card-crt")->IsVisible(true), "CRT filter is on the Display tab");
    demand(ui.draft.rendering.crt_filter == jfg::CrtPreset::off &&
               element("video-crt-off")->IsClassSet("selected") &&
               !element("video-crt-rgb-monitor")->IsClassSet("selected"),
           "old file shows CRT filter Off");
    demand(disabled("video-crt-off") && disabled("video-crt-rgb-monitor"),
           "CRT filter is disabled under Enhanced");
    pointer("video-crt-rgb-monitor");
    demand(ui.draft.rendering.crt_filter == jfg::CrtPreset::off && !ui.dirty(),
           "a disabled CRT filter ignores clicks");

    pointer("video-output-original");
    demand(!disabled("video-crt-off") && !disabled("video-crt-rgb-monitor") &&
               element("video-crt-off")->GetComputedValues().opacity() == 1.0f,
           "CRT filter is enabled under Original");
    pointer("video-crt-rgb-monitor");
    demand(ui.draft.rendering.crt_filter == jfg::CrtPreset::rgb_monitor &&
               element("video-crt-rgb-monitor")->IsClassSet("selected") &&
               !element("video-crt-off")->IsClassSet("selected"),
           "RGB monitor is selected");
    surface->context->ProcessMouseMove(0, 0, 0);
    capture("display-crt.tga");
    ui.running = true;
    ui.refresh();
    settle();
    demand(element("video-restart")->IsVisible(true), "CRT filter needs a game restart");
    ui.running = false;

    pointer("video-output-enhanced");
    demand(disabled("video-crt-rgb-monitor") &&
               ui.draft.rendering.crt_filter == jfg::CrtPreset::rgb_monitor &&
               element("video-crt-rgb-monitor")->IsClassSet("selected"),
           "Enhanced keeps the CRT preset, disabled");
    demand(element("video-crt-rgb-monitor")->GetComputedValues().opacity() < 1.0f &&
               element("video-crt-off")->GetComputedValues().opacity() < 1.0f,
           "a disabled CRT filter is greyed out");
    capture("display-crt-disabled.tga");
    pointer("video-apply");
    accepted();
    {
      const auto stored = jfg::load_video_settings(profile / "video.ini");
      demand(stored && stored->crt_filter == jfg::CrtPreset::rgb_monitor &&
                 stored->output == jfg::VideoOutput::enhanced,
             "Apply persists the CRT preset while it is disabled");
    }

    auto *search = static_cast<Rml::ElementFormControlInput *>(element("video-search"));
    search->SetValue("scanlines");
    Rml::Dictionary query;
    query["value"] = std::string("scanlines");
    search->DispatchEvent("change", query);
    settle();
    demand(element("video-card-crt")->IsVisible(true) && !element("video-card-fog")->IsVisible(true),
           "search finds the CRT filter");

    pointer("video-tab-display");
    {
      std::string order;
      for (auto *button = element("video-crt-off"); button; button = button->GetNextSibling())
        order += button->GetId() + " ";
      demand(order == "video-crt-off video-crt-composite-tv video-crt-svideo-tv video-crt-rgb-monitor ",
             "CRT presets show as Off / Composite TV / S-Video TV / RGB monitor");
    }
    pointer("video-output-original");
    for (const auto &[id, preset, image] :
         {std::tuple{"video-crt-composite-tv", jfg::CrtPreset::composite_tv, "display-crt-composite.tga"},
          std::tuple{"video-crt-svideo-tv", jfg::CrtPreset::svideo_tv, "display-crt-svideo.tga"}}) {
      pointer(id);
      demand(ui.draft.rendering.crt_filter == preset && element(id)->IsClassSet("selected") &&
                 !element("video-crt-rgb-monitor")->IsClassSet("selected"),
             "a TV preset is selected");
      surface->context->ProcessMouseMove(0, 0, 0);
      capture(image);
      pointer("video-apply");
      accepted();
      demand(jfg::load_video_settings(profile / "video.ini")->crt_filter == preset, "Apply persists a TV preset");
    }
    pointer("video-crt-off");
    pointer("video-apply");
    accepted();
    demand(jfg::load_video_settings(profile / "video.ini")->crt_filter == jfg::CrtPreset::off,
           "Apply persists CRT filter Off");
    std::ofstream(home / "result.json")
        << "{\"passed\":true,\"checks\":" << checks
        << ",\"physical_desktop_input\":false}";
    FrontendUiShutdown();
    ready = false;
    DestroyWindow(window);
    return 0;
  } catch (const std::exception &e) {
    if (!home.empty())
      std::ofstream(home / "result.json")
          << "{\"passed\":false,\"checks\":" << checks << ",\"error\":\""
          << escape(e.what()) << "\"}";
    if (ready)
      FrontendUiShutdown();
    if (window)
      DestroyWindow(window);
    return 1;
  }
}
