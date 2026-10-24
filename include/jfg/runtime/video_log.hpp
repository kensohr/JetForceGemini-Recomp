#pragma once
#include <string_view>
#if defined(_WIN32)
#include "jfg/runtime/support_snapshot.hpp"
#include <mutex>
#endif

namespace jfg {
// Mirrors the "Original video output: ..." log lines into video.log next to
// the support log that JFG_SUPPORT_LOG names, so they can be read while the
// game runs (the launcher keeps the game's stderr in an unshared temp file).
// The lines carry no guest bytes, paths or personal data.
//
// The first line a process writes truncates the file, so it holds one run.
// Each line is opened, written, flushed and closed so it survives a crash,
// and the file is shared for reading. Callers include RT64's workload thread
// (coverage log sink) and the main thread, so writes are serialised.
// Without JFG_SUPPORT_LOG, or when the file can't be opened, nothing happens.
inline void append_video_log_line(const std::string_view line) noexcept {
#if defined(_WIN32)
    static std::mutex mutex;
    static bool has_started_run = false;
    const std::lock_guard<std::mutex> lock(mutex);
    wchar_t path[32768]{};
    if (!support_sibling(L"video.log", path)) return;
    const DWORD disposition = has_started_run ? OPEN_ALWAYS : CREATE_ALWAYS;
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    has_started_run = true;
    LARGE_INTEGER end{};
    if (SetFilePointerEx(file, end, nullptr, FILE_END)) {
        DWORD written = 0;
        (void)WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        (void)FlushFileBuffers(file);
    }
    CloseHandle(file);
#else
    (void)line;
#endif
}
} // namespace jfg
