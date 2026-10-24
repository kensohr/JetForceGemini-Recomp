#pragma once

// Where the video core reports to the project: coverage statuses and VI
// settings it can't show. A project sets the sink once, before RT64 starts
// rendering; while it is unset, reports are dropped. Each step keeps its own
// once-per-run rule.
namespace n64_video {

enum class LogLevel {
    debug,
    info,
    warning,
};

using LogSink = void (*)(LogLevel level, const char *message);

inline LogSink &log_sink() {
    static LogSink sink = nullptr;
    return sink;
}

} // namespace n64_video
