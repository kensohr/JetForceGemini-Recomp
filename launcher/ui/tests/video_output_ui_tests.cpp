// Video output card of the RmlUi video settings page. Kept apart from
// video_settings_ui_tests.cpp so upstream's test file stays unchanged. The
// helpers below copy that file's: upstream's UI tests each define their own,
// and sharing them would mean editing upstream's file to include a header.
// Usage: jfg_video_output_ui_tests.exe <new-artifact-directory> [--no-captures].
#include "../native_ui.cpp"
#include <stdexcept>
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
static void click(const char *id) {
  auto *e = element(id);
  demand(!e->HasAttribute("disabled"), id);
  e->Click();
  settle();
}
// Clicks like a pointer would, first switching to the tab that shows the
// control when it is hidden.
static void pointer(const char *id) {
  auto *e = element(id);
  for (auto *parent = e; parent; parent = parent->GetParentNode()) {
    if (parent->GetId() == "video-card-output" && !parent->IsVisible(true)) {
      click("video-tab-display");
      break;
    }
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

static void check_video_output_protocol() {
  jfg::frontend::VideoRequest request;
  request.version = 4;
  demand(!jfg::frontend::valid_video_request(request), "reject protocol without video output");
  request = {};
  demand(request.version == 5 && request.video_output == 0, "default request carries Enhanced");
  request.video_output = 1;
  demand(jfg::frontend::valid_video_request(request), "Original video output protocol valid");
  request.video_output = 2;
  demand(!jfg::frontend::valid_video_request(request), "reject invalid video output protocol");
}

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
        L"JfgRmlVideoOutputChecks-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0,
                                   GENERIC_ALL, nullptr);
    demand(desktop && SetThreadDesktop(desktop), "isolated inactive desktop");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = instance;
    cls.lpszClassName = L"JfgRmlVideoOutputTests";
    RegisterClassW(&cls);
    window = CreateWindowExW(0, cls.lpszClassName, L"Video Settings",
                             WS_OVERLAPPEDWINDOW, 40, 40, 720, 600, nullptr,
                             nullptr, instance, nullptr);
    const auto profile = home / "profile";
    fs::create_directories(profile);
    // A file written before the setting existed.
    std::ofstream(profile / "video.ini")
        << "version=1\nmode=custom\nfps=60\nvsync=1\n";
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
    check_video_output_protocol();
    MSG close{};
    auto accepted = [&] {
      demand(PeekMessageW(&close, window, WM_CLOSE, WM_CLOSE, PM_REMOVE) != 0,
             "Apply accepts and closes");
    };
    pointer("video-tab-display");
    demand(element("video-card-output")->IsVisible(true), "Video output is on the Display tab");
    demand(ui.draft.rendering.output == jfg::VideoOutput::enhanced &&
               element("video-output-enhanced")->IsClassSet("selected") &&
               !element("video-output-original")->IsClassSet("selected"),
           "old file shows Enhanced video output");
    pointer("video-output-original");
    demand(ui.draft.rendering.output == jfg::VideoOutput::original && ui.dirty() &&
               element("video-output-original")->IsClassSet("selected") &&
               element("video-dirty-display")->IsVisible(true),
           "Original video output changes the Display draft");
    // Move the hover away so the screenshot shows the selection alone.
    surface->context->ProcessMouseMove(0, 0, 0);
    capture("display.tga");
    ui.running = true;
    ui.refresh();
    settle();
    demand(element("video-restart")->IsVisible(true), "video output needs a game restart");
    ui.running = false;
    pointer("video-output-enhanced");
    demand(!ui.dirty(), "returning to Enhanced clears dirty state");
    auto *search = static_cast<Rml::ElementFormControlInput *>(element("video-search"));
    search->SetValue("native resolution");
    Rml::Dictionary outputQuery;
    outputQuery["value"] = std::string("native resolution");
    search->DispatchEvent("change", outputQuery);
    settle();
    demand(element("video-card-output")->IsVisible(true) && !element("video-card-fog")->IsVisible(true),
           "search finds video output");
    pointer("video-tab-display");
    pointer("video-output-original");
    pointer("video-apply");
    accepted();
    demand(jfg::load_video_settings(profile / "video.ini")->output == jfg::VideoOutput::original,
           "Apply persists Original video output");
    pointer("video-output-enhanced");
    pointer("video-apply");
    accepted();
    demand(jfg::load_video_settings(profile / "video.ini")->output == jfg::VideoOutput::enhanced,
           "Apply persists Enhanced video output");
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
