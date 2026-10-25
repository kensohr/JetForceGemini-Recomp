// CRT filter setting: video.ini round trip, the default for files written
// before the setting existed, rejection of malformed values and the rule that
// the setting only takes effect under Original video output. Kept apart from
// video_settings_tests.cpp so upstream's test file stays unchanged.
#include "jfg/renderer/crt_filter_settings.hpp"
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
  // A file written before the setting existed.
  const std::string legacy = "version=1\nmode=custom\nfps=120\nvsync=1\nvideo_output=original\n";
  const auto migrated = jfg::parse_video_settings(legacy);
  check(migrated && migrated->crt_filter == jfg::CrtPreset::off, "missing key reads as Off");
  check(jfg::VideoSettings{}.crt_filter == jfg::CrtPreset::off, "default is Off");

  for (const auto &choice : jfg::crt_presets) {
    jfg::VideoSettings settings{};
    settings.output = jfg::VideoOutput::original;
    settings.crt_filter = choice.preset;
    const auto restored = jfg::parse_video_settings(jfg::serialize_video_settings(settings));
    check(restored && restored->crt_filter == choice.preset, "every CRT preset round-trips");
  }
  {
    // Enhanced keeps the stored preset; it only stops taking effect.
    jfg::VideoSettings settings{};
    settings.crt_filter = jfg::CrtPreset::rgb_monitor;
    const auto restored = jfg::parse_video_settings(jfg::serialize_video_settings(settings));
    check(restored && restored->crt_filter == jfg::CrtPreset::rgb_monitor &&
              restored->output == jfg::VideoOutput::enhanced,
          "Enhanced keeps the stored CRT preset");
  }
  check(jfg::serialize_video_settings(jfg::VideoSettings{}).find("\ncrt_filter=off\n") != std::string::npos,
        "Off is written as crt_filter=off");
  {
    const auto parsed = jfg::parse_video_settings(legacy + "crt_filter=rgb_monitor\n");
    check(parsed && parsed->crt_filter == jfg::CrtPreset::rgb_monitor, "rgb_monitor parses");
  }
  for (const auto *bad : {"crt_filter=\n", "crt_filter=RGB_monitor\n", "crt_filter=1\n",
                          "crt_filter=off\ncrt_filter=rgb_monitor\n"})
    check(!jfg::parse_video_settings(legacy + bad), "malformed CRT filter value is rejected");
  {
    jfg::VideoSettings invalid{};
    invalid.crt_filter = static_cast<jfg::CrtPreset>(jfg::crt_presets.size());
    check(!jfg::valid_video_settings(invalid), "out-of-range CRT preset is invalid");
  }
  check(jfg::crt_presets.front().preset == jfg::CrtPreset::off, "Off is listed first");
  check(jfg::crt_filter_available(jfg::VideoOutput::original), "available under Original");
  check(!jfg::crt_filter_available(jfg::VideoOutput::enhanced), "unavailable under Enhanced");
  check(jfg::effective_crt_preset({.output = jfg::VideoOutput::enhanced,
                                   .crt_filter = jfg::CrtPreset::rgb_monitor}) == jfg::CrtPreset::off,
        "Enhanced ignores the stored CRT preset");
  check(jfg::effective_crt_preset({.output = jfg::VideoOutput::original,
                                   .crt_filter = jfg::CrtPreset::rgb_monitor}) == jfg::CrtPreset::rgb_monitor,
        "Original applies the stored CRT preset");
  std::cout << "CRT filter settings checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
