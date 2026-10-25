// An unknown crt_curvature or crt_filter value in video.ini reads as the
// default, and the runtime logs a warning about it with the other
// "Original video output: ..." lines.
#include "video_settings_warnings_log.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

std::string read_all(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

} // namespace

int main() {
  unsigned failures = 0;
  auto check = [&](bool ok, const char *name) {
    if (!ok) {
      ++failures;
      std::cerr << "FAIL: " << name << '\n';
    }
  };
  const auto directory = std::filesystem::temp_directory_path() / L"jfg-video-settings-warnings-log-tests";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  SetEnvironmentVariableW(L"JFG_SUPPORT_LOG", (directory / L"native.log").c_str());
  const auto video_ini = directory / L"video.ini";
  const auto video_log = directory / L"video.log";

  std::ofstream(video_ini, std::ios::binary)
      << "version=1\nmode=original\nfps=60\nvsync=1\nvideo_output=original\ncrt_filter=rgb_monitor\ncrt_curvature=1\n";
  jfg::log_video_settings_warnings(video_ini);
  check(!std::filesystem::exists(video_log) || read_all(video_log).empty(), "a known value logs nothing");

  std::ofstream(video_ini, std::ios::binary)
      << "version=1\nmode=original\nfps=60\nvsync=1\nvideo_output=original\ncrt_filter=rgb_monitor\ncrt_curvature=on\n";
  jfg::log_video_settings_warnings(video_ini);
  check(read_all(video_log) ==
            "[warning] Original video output: video.ini crt_curvature is not 0 or 1; screen curvature reads as off\n",
        "an unknown value logs one warning");

  std::filesystem::remove(video_log);
  std::ofstream(video_ini, std::ios::binary)
      << "version=1\nmode=original\nfps=60\nvsync=1\nvideo_output=original\ncrt_filter=pal_tv\ncrt_curvature=1\n";
  jfg::log_video_settings_warnings(video_ini);
  check(read_all(video_log) ==
            "[warning] Original video output: video.ini crt_filter is not a known CRT preset; the CRT filter reads as Off\n",
        "an unknown CRT filter value logs one warning");

  jfg::log_video_settings_warnings(directory / L"missing.ini");
  check(read_all(video_log).find("missing") == std::string::npos, "a missing file logs nothing");

  std::filesystem::remove_all(directory);
  std::cout << "video.ini warnings log checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
#else
int main() { return 0; }
#endif
