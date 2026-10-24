// The "Original video output: ..." lines are mirrored into video.log next to
// the support log that JFG_SUPPORT_LOG names, so they can be read during play.
#include "original_video_output_log.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void lines_replace_previous_run_and_keep_order(const std::filesystem::path& directory) {
    const auto video_log = directory / L"video.log";
    std::ofstream(video_log, std::ios::binary) << "[info] Original video output: previous run\n";
    SetEnvironmentVariableW(L"JFG_SUPPORT_LOG", (directory / L"native.log").c_str());

    jfg::log_original_video_output(jfg::LogLevel::info, "MSAA 4x");
    jfg::log_original_video_output(jfg::LogLevel::warning, "coverage unavailable");
    jfg::log_original_video_output(jfg::LogLevel::debug, "coverage estimating");

    assert(read_all(video_log) ==
        "[info] Original video output: MSAA 4x\n"
        "[warning] Original video output: coverage unavailable\n"
        "[debug] Original video output: coverage estimating\n");
}

// The developer opens video.log while the game runs, so the writer must keep
// appending while a reader holds the file open.
void file_stays_readable_while_written(const std::filesystem::path& directory) {
    const auto video_log = directory / L"video.log";
    const HANDLE reader = CreateFileW(video_log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(reader != INVALID_HANDLE_VALUE);

    jfg::log_original_video_output(jfg::LogLevel::info, "written while open");

    char contents[4096]{};
    DWORD read = 0;
    assert(ReadFile(reader, contents, sizeof(contents) - 1, &read, nullptr));
    CloseHandle(reader);
    assert(std::string(contents, read).ends_with("[info] Original video output: written while open\n"));
}

// The coverage sink logs from RT64's workload thread while the main thread
// logs too; every line must arrive whole.
void concurrent_lines_all_arrive_whole(const std::filesystem::path& directory) {
    const auto video_log = directory / L"video.log";
    const std::string before = read_all(video_log);
    constexpr int lines_per_thread = 300;
    const auto log_lines = [](const char* thread_name) {
        for (int index = 0; index < lines_per_thread; ++index)
            jfg::log_original_video_output(jfg::LogLevel::debug,
                std::string(thread_name) + " line " + std::to_string(index));
    };
    std::thread workload(log_lines, "workload");
    std::thread main_thread(log_lines, "main");
    workload.join();
    main_thread.join();

    const std::string contents = read_all(video_log);
    assert(contents.starts_with(before));
    std::istringstream added(contents.substr(before.size()));
    std::multiset<std::string> lines;
    for (std::string line; std::getline(added, line);) lines.insert(line);
    std::multiset<std::string> expected;
    for (const char* thread_name : {"workload", "main"})
        for (int index = 0; index < lines_per_thread; ++index)
            expected.insert(std::string("[debug] Original video output: ") + thread_name + " line " +
                            std::to_string(index));
    assert(lines == expected);
}

void nothing_written_without_support_log(const std::filesystem::path& directory) {
    const auto video_log = directory / L"video.log";
    const std::string before = read_all(video_log);
    SetEnvironmentVariableW(L"JFG_SUPPORT_LOG", nullptr);

    jfg::log_original_video_output(jfg::LogLevel::info, "no support folder");

    assert(read_all(video_log) == before);
    assert(std::distance(std::filesystem::directory_iterator(directory),
                         std::filesystem::directory_iterator()) == 1);
}

}  // namespace
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    assert(argc == 2);
    const std::filesystem::path directory(argv[1]);
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    lines_replace_previous_run_and_keep_order(directory);
    file_stays_readable_while_written(directory);
    concurrent_lines_all_arrive_whole(directory);
    nothing_written_without_support_log(directory);
#else
    (void)argc;
    (void)argv;
    jfg::log_original_video_output(jfg::LogLevel::info, "non-Windows builds log to stderr only");
#endif
    return 0;
}
