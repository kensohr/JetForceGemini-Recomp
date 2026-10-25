// Screen curvature setting: video.ini round trip, the default for files
// written before the setting existed, unknown values reading as off with a
// warning, and the rule that it only applies while a CRT filter runs. Kept
// apart from video_settings_tests.cpp so upstream's test file stays unchanged.
#include "jfg/renderer/crt_curvature_settings.hpp"
#include <iostream>
#include <string>

int main() {
  unsigned failures = 0;
  auto check = [&](bool ok, const char *name) {
    if (!ok) {
      ++failures;
      std::cerr << "FAIL: " << name << '\n';
    }
  };
  const std::string legacy =
      "version=1\nmode=custom\nfps=120\nvsync=1\nvideo_output=original\ncrt_filter=rgb_monitor\n";
  {
    const auto migrated = jfg::parse_video_settings(legacy);
    check(migrated && !migrated->crt_curvature, "missing key reads as off");
    check(!jfg::VideoSettings{}.crt_curvature, "default is off");
    check(jfg::video_settings_warnings(legacy).empty(), "a file without the key has no warning");
  }
  for (const bool on : {false, true}) {
    jfg::VideoSettings settings{};
    settings.output = jfg::VideoOutput::original;
    settings.crt_filter = jfg::CrtPreset::rgb_monitor;
    settings.crt_curvature = on;
    const auto text = jfg::serialize_video_settings(settings);
    const auto restored = jfg::parse_video_settings(text);
    check(restored && restored->crt_curvature == on, "crt_curvature round-trips");
    check(jfg::video_settings_warnings(text).empty(), "a written file has no warning");
  }
  check(jfg::serialize_video_settings(jfg::VideoSettings{}).find("\ncrt_curvature=0\n") != std::string::npos,
        "off is written as crt_curvature=0");
  {
    const auto parsed = jfg::parse_video_settings(legacy + "crt_curvature=1\n");
    check(parsed && parsed->crt_curvature, "crt_curvature=1 parses as on");
  }
  for (const auto *unknown : {"crt_curvature=\n", "crt_curvature=yes\n", "crt_curvature=2\n"}) {
    const std::string text = legacy + unknown;
    const auto parsed = jfg::parse_video_settings(text);
    check(parsed && !parsed->crt_curvature && parsed->crt_filter == jfg::CrtPreset::rgb_monitor,
          "an unknown value reads as off and keeps the rest of the file");
    const auto warnings = jfg::video_settings_warnings(text);
    check(warnings.size() == 1 && warnings.front().find("crt_curvature") != std::string::npos,
          "an unknown value gives one warning naming the key");
  }
  check(!jfg::parse_video_settings(legacy + "crt_curvature=0\ncrt_curvature=1\n"),
        "a repeated key is rejected like the other keys");

  // Availability: only while a CRT filter is selected under Original.
  const jfg::VideoSettings crt_on{.output = jfg::VideoOutput::original,
                                  .crt_filter = jfg::CrtPreset::rgb_monitor,
                                  .crt_curvature = true};
  check(jfg::crt_curvature_available(crt_on), "available with a CRT filter under Original");
  check(jfg::effective_crt_curvature(crt_on), "applies with a CRT filter under Original");
  auto filter_off = crt_on;
  filter_off.crt_filter = jfg::CrtPreset::off;
  check(!jfg::crt_curvature_available(filter_off), "unavailable with the CRT filter off");
  check(!jfg::effective_crt_curvature(filter_off), "ignored with the CRT filter off");
  auto enhanced = crt_on;
  enhanced.output = jfg::VideoOutput::enhanced;
  check(!jfg::crt_curvature_available(enhanced), "unavailable under Enhanced");
  check(!jfg::effective_crt_curvature(enhanced), "ignored under Enhanced");
  {
    // Disabled keeps the stored value.
    const auto restored = jfg::parse_video_settings(jfg::serialize_video_settings(enhanced));
    check(restored && restored->crt_curvature, "a disabled setting keeps its stored value");
  }
  std::cout << "Screen curvature settings checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
