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
#include "jfg/runtime/log.hpp"
#include "n64_video/rt64_configuration.hpp"

#include "common/rt64_user_configuration.h"
#include "render/rt64_render_target.h"
#include "rhi/rt64_render_hooks.h"

#include <iostream>
#include <string>

namespace jfg {

struct MsaaDeviceLimit {
    RT64::UserConfiguration* configuration = nullptr;
    RT64::RenderHookInit* previous_init = nullptr;
};

inline MsaaDeviceLimit& pending_msaa_device_limit() {
    static MsaaDeviceLimit limit;
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
                            unsigned supported_sample_counts, bool sample_locations_supported) {
    const std::string device_support = "device sample counts " +
        sample_count_list(supported_sample_counts) + ", sample locations " +
        (sample_locations_supported ? "yes" : "no");
    if (sample_count == requested_sample_count) {
        write_log(std::cerr, LogLevel::info,
            "Original video output: MSAA " + std::to_string(sample_count) + "x in use");
        return;
    }
    if (sample_count > n64_video::msaa_off_sample_count) {
        write_log(std::cerr, LogLevel::warning,
            "Original video output: device lacks MSAA " + std::to_string(requested_sample_count) +
            "x; MSAA " + std::to_string(sample_count) + "x in use (" + device_support + ")");
        return;
    }
    write_log(std::cerr, LogLevel::warning,
        "Original video output: MSAA unavailable on this device; edges not anti-aliased (" +
        device_support + ")");
}

inline void limit_msaa_at_render_init(RenderInterface* render_interface, RenderDevice* device) {
    MsaaDeviceLimit limit = pending_msaa_device_limit();
    pending_msaa_device_limit() = {};
    RT64::SetRenderHooks(limit.previous_init, RT64::GetRenderHookDraw(),
        RT64::GetRenderHookDeinit());
    if (limit.configuration != nullptr && device != nullptr) {
        RT64::UserConfiguration& configuration = *limit.configuration;
        const bool uses_hdr = rt64_uses_hdr(configuration, *device);
        const RenderSampleCounts supported_sample_counts =
            device->getSampleCountsSupported(RT64::RenderTarget::colorBufferFormat(uses_hdr)) &
            device->getSampleCountsSupported(RT64::RenderTarget::depthBufferFormat());
        const bool sample_locations_supported = device->getCapabilities().sampleLocations;
        const unsigned requested_sample_count = configuration.msaaSampleCount();
        const unsigned sample_count = n64_video::limit_msaa_to_device(
            configuration, supported_sample_counts, sample_locations_supported);
        log_msaa_in_use(requested_sample_count, sample_count, supported_sample_counts,
            sample_locations_supported);
    }
    if (limit.previous_init != nullptr)
        limit.previous_init(render_interface, device);
}

// Call after apply_to_rt64 and before RT64::Application::setup, once the
// shell's own render hooks are installed. Does nothing when no MSAA was
// requested.
inline void limit_msaa_to_device_at_setup(RT64::UserConfiguration& configuration) {
    if (configuration.antialiasing == RT64::UserConfiguration::Antialiasing::None)
        return;
    pending_msaa_device_limit() = {&configuration, RT64::GetRenderHookInit()};
    RT64::SetRenderHooks(&limit_msaa_at_render_init, RT64::GetRenderHookDraw(),
        RT64::GetRenderHookDeinit());
}

} // namespace jfg
