#pragma once

// Project glue: limits the MSAA that Original video output requests to what
// the graphics device supports, without patching RT64.
//
// RT64 creates the device inside Application::setup and then checks MSAA
// support itself, but it only turns an unsupported count off (no 4x
// fallback), does so silently, and doesn't notice a device without custom
// sample locations, where it renders without MSAA while the configuration
// still claims it. Between creating the device and that check, setup calls
// RT64's render init hook. This file borrows that hook for one call: it
// lowers the configured count with n64_video::limit_msaa_to_device, logs the
// count in use, then puts the previous hooks back and chains to the previous
// init hook (the shell's capture hook), so RT64 sees a supported count.
//
// It borrows the deinit hook too: when setup fails before calling the init
// hook, Application::end still calls the deinit hook, which drops the pending
// limit so no pointer to the destroyed configuration outlives it.
#include "jfg/runtime/log.hpp"
#include "n64_video/rt64_configuration.hpp"

#include "common/rt64_user_configuration.h"
#include "render/rt64_render_target.h"
#include "rhi/rt64_render_hooks.h"

#include <iostream>
#include <string>
#include <string_view>

namespace jfg {

inline constexpr std::string_view original_video_output_log_prefix =
    "Original video output: ";

inline void log_original_video_output(LogLevel level, const std::string& message) {
    write_log(std::cerr, level, std::string(original_video_output_log_prefix) + message);
}

struct MsaaDeviceLimit {
    RT64::UserConfiguration* configuration = nullptr;
    RT64::RenderHookInit* previous_init = nullptr;
    RT64::RenderHookDeinit* previous_deinit = nullptr;
};

inline MsaaDeviceLimit& pending_msaa_device_limit() {
    static MsaaDeviceLimit limit;
    return limit;
}

// Clears the pending limit and puts back the hooks it replaced; returns the
// limit that was pending.
inline MsaaDeviceLimit take_pending_msaa_device_limit() {
    const MsaaDeviceLimit limit = pending_msaa_device_limit();
    pending_msaa_device_limit() = {};
    RT64::SetRenderHooks(limit.previous_init, RT64::GetRenderHookDraw(),
        limit.previous_deinit);
    return limit;
}

// Mirrors Application::setup's choice of colour format, which RT64 doesn't
// expose before its own MSAA check.
inline bool rt64_uses_hdr(const RT64::UserConfiguration& configuration,
                          const RenderDevice& device) {
    switch (configuration.internalColorFormat) {
    case RT64::UserConfiguration::InternalColorFormat::High:
        return true;
    case RT64::UserConfiguration::InternalColorFormat::Automatic:
        return device.getCapabilities().preferHDR;
    case RT64::UserConfiguration::InternalColorFormat::Standard:
    default:
        return false;
    }
}

inline n64_video::MsaaSupport device_msaa_support(
    const RT64::UserConfiguration& configuration, RenderDevice& device) {
    const bool uses_hdr = rt64_uses_hdr(configuration, device);
    n64_video::MsaaSupport support;
    support.sample_counts =
        device.getSampleCountsSupported(RT64::RenderTarget::colorBufferFormat(uses_hdr)) &
        device.getSampleCountsSupported(RT64::RenderTarget::depthBufferFormat());
    support.sample_locations = device.getCapabilities().sampleLocations;
    return support;
}

// Lists the counts in a sample-count bit mask, e.g. "1 2 4 8".
inline std::string sample_count_list(unsigned sample_counts) {
    constexpr unsigned highest_listed_count = 64;
    std::string list;
    for (unsigned count = 1; count <= highest_listed_count; count <<= 1) {
        if ((sample_counts & count) == 0)
            continue;
        if (!list.empty())
            list += ' ';
        list += std::to_string(count);
    }
    return list.empty() ? std::string("none") : list;
}

inline void log_msaa_in_use(unsigned requested_sample_count, unsigned sample_count,
                            n64_video::MsaaSupport support) {
    const std::string device_support = "device sample counts " +
        sample_count_list(support.sample_counts) + ", sample locations " +
        (support.sample_locations ? "yes" : "no");
    if (sample_count == requested_sample_count) {
        log_original_video_output(LogLevel::info,
            "MSAA " + std::to_string(sample_count) + "x in use");
        return;
    }
    if (sample_count > n64_video::msaa_off_sample_count) {
        log_original_video_output(LogLevel::warning,
            "device lacks MSAA " + std::to_string(requested_sample_count) +
            "x; MSAA " + std::to_string(sample_count) + "x in use (" + device_support + ")");
        return;
    }
    log_original_video_output(LogLevel::warning,
        "MSAA unavailable on this device; edges not anti-aliased (" + device_support + ")");
}

inline void limit_configured_msaa_to_device(RT64::UserConfiguration& configuration, RenderDevice& device) {
    const n64_video::MsaaSupport support = device_msaa_support(configuration, device);
    const unsigned requested_sample_count = configuration.msaaSampleCount();
    const unsigned sample_count = n64_video::limit_msaa_to_device(configuration, support);
    log_msaa_in_use(requested_sample_count, sample_count, support);
}

inline void limit_msaa_at_render_init(RenderInterface* render_interface, RenderDevice* device) {
    const MsaaDeviceLimit limit = take_pending_msaa_device_limit();
    if (limit.configuration != nullptr && device != nullptr) {
        limit_configured_msaa_to_device(*limit.configuration, *device);
    }
    else if (limit.configuration != nullptr) {
        // RT64 leaves its own MSAA check to run against the requested count.
        log_original_video_output(LogLevel::warning,
            "no graphics device at render init; MSAA not limited to the device");
    }
    if (limit.previous_init != nullptr)
        limit.previous_init(render_interface, device);
}

// Reached only when setup ended before the init hook ran.
inline void drop_msaa_limit_at_render_deinit() {
    const MsaaDeviceLimit limit = take_pending_msaa_device_limit();
    log_original_video_output(LogLevel::debug,
        "renderer ended before render init; MSAA device limit dropped");
    if (limit.previous_deinit != nullptr)
        limit.previous_deinit();
}

// Call after apply_to_rt64 and before RT64::Application::setup, once the
// shell's own render hooks are installed. Does nothing when no MSAA was
// requested.
inline void limit_msaa_to_device_at_setup(RT64::UserConfiguration& configuration) {
    if (configuration.antialiasing == RT64::UserConfiguration::Antialiasing::None)
        return;
    pending_msaa_device_limit() = {&configuration, RT64::GetRenderHookInit(),
        RT64::GetRenderHookDeinit()};
    RT64::SetRenderHooks(&limit_msaa_at_render_init, RT64::GetRenderHookDraw(),
        &drop_msaa_limit_at_render_deinit);
}

} // namespace jfg
