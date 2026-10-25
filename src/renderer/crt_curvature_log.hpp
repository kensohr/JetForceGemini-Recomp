#pragma once

// Project glue: logs the video.ini values that read as a default because
// they are unknown (crt_curvature reads as off, crt_filter as Off), with the other
// "Original video output: ..." lines, once when the runtime loads the file.
#include "jfg/renderer/crt_curvature_settings.hpp"
#include "original_video_output_log.hpp"

#include <filesystem>

namespace jfg {

inline void log_video_settings_warnings(const std::filesystem::path &video_ini) {
    for (const auto &warning : load_video_settings_warnings(video_ini))
        log_original_video_output(LogLevel::warning, warning);
}

} // namespace jfg
