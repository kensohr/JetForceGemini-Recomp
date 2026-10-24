// Video output setting: video.ini round trip, legacy default and rejection of
// malformed values. Kept apart from video_settings_tests.cpp so upstream's
// test file stays unchanged.
#include "jfg/renderer/video_settings.hpp"
#include <iostream>
#include <string>

int main() {
  unsigned failures = 0;
  auto check = [&](bool ok) {
    if (!ok)
      ++failures;
  };
  // A file written before the setting existed.
  const std::string legacy = "version=1\nmode=custom\nfps=120\nvsync=1\n";
  const auto migrated = jfg::parse_video_settings(legacy);
  check(migrated && migrated->output == jfg::VideoOutput::enhanced);
  for (auto output : {jfg::VideoOutput::enhanced, jfg::VideoOutput::original}) {
    jfg::VideoSettings choice{};
    choice.output = output;
    const auto restored = jfg::parse_video_settings(jfg::serialize_video_settings(choice));
    check(restored && restored->output == output);
  }
  check(jfg::parse_video_settings(legacy + "video_output=original\n")->output ==
        jfg::VideoOutput::original);
  for (const auto *bad : {"video_output=\n", "video_output=Original\n", "video_output=native\n",
                          "video_output=original\nvideo_output=enhanced\n"})
    check(!jfg::parse_video_settings(legacy + bad));
  {
    jfg::VideoSettings invalid{};
    invalid.output = static_cast<jfg::VideoOutput>(2);
    check(!jfg::valid_video_settings(invalid));
  }
  std::cout << "Video output settings checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
