# Video core (src/renderer/n64_video) RT64 coverage patch, option B of
# Original video output's anti-aliasing: estimates each pixel's RDP coverage
# and front-surface colour from the MSAA samples, and the VI pre-pass runs the
# VI's own anti-aliasing and divot filter on them.
#
# It applies on top of the video core's VI-pass patch (rt64_n64_video.cmake),
# so a project can take that patch alone. Its pieces:
# - the raster shader writes a per-sample render-mode record into the colour
#   target's alpha (CoverageRecord.hlsli), switched on by a frame parameter;
# - MSAA uses the RDP's sample positions (RasterShader);
# - after each framebuffer pair the workload thread runs CoverageEstimateCS
#   and keeps the result with the colour target (rt64_coverage_estimate.*),
#   copied to the interpolation target it's resolved into;
# - the present queue hands the estimate to the VI pre-pass, which reads it
#   instead of the averaged frame.
#
# Every patched file is checked against the pinned RT64 input (line endings
# normalised) and each patch site must exist in the version being patched, so
# a changed RT64 file or an earlier patch that moved a site fails the build.
# New RT64 files come from src/renderer/n64_video/rt64_patch/.

# Replaces one patch site in `contents`; a missing site fails the build.
function(jfg_vi_coverage_replace before after)
    string(FIND "${contents}" "${before}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "Missing VI coverage RT64 patch site: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" contents "${contents}")
    set(contents "${contents}" PARENT_SCOPE)
endfunction()

function(jfg_vi_coverage_split_pair pair first_variable second_variable)
    string(REPLACE "|" ";" fields "${pair}")
    list(GET fields 0 first)
    list(GET fields 1 second)
    set(${first_variable} "${first}" PARENT_SCOPE)
    set(${second_variable} "${second}" PARENT_SCOPE)
endfunction()

# Fails the build unless `relative` in the pinned RT64 checkout still has the
# audited content (LF-normalised SHA-256).
function(jfg_vi_coverage_check_input original_root relative expected)
    file(READ "${original_root}/${relative}" pinned)
    string(REPLACE "\r\n" "\n" pinned "${pinned}")
    string(SHA256 digest "${pinned}")
    if(NOT digest STREQUAL expected)
        message(FATAL_ERROR "Pinned VI coverage RT64 input changed: ${relative}")
    endif()
endfunction()

function(jfg_vi_coverage_write destination contents)
    set(existing "")
    if(EXISTS "${destination}")
        file(READ "${destination}" existing)
    endif()
    if(NOT existing STREQUAL contents)
        file(WRITE "${destination}" "${contents}")
    endif()
endfunction()

# Applies the patches for `relative` to `contents`.
function(jfg_vi_coverage_patch_contents relative)
    if(relative STREQUAL "src/shared/rt64_frame_params.h")
        jfg_vi_coverage_replace([==[        float ditherNoiseStrength;]==] [==[        float ditherNoiseStrength;
        // Video core coverage estimate: 1 makes the raster shader write the
        // per-sample render-mode record into the colour target's alpha.
        uint coverageRecord;]==])
    elseif(relative STREQUAL "src/shaders/RasterPS.hlsl")
        jfg_vi_coverage_replace([==[#include "TextureSampler.hlsli"]==] [==[#include "TextureSampler.hlsli"
#include "CoverageRecord.hlsli"]==])
        jfg_vi_coverage_replace([==[    // Add highlight color to the last step.]==] [==[    // Video core coverage estimate: record what the RDP would do with this
    // sample's coverage.
    if (FrParams.coverageRecord != 0) {
        resultColor.a = CoverageRecordAlpha(otherMode, resultColor.a, combinerColor.a, cvgRange);
    }

    // Add highlight color to the last step.]==])
    elseif(relative STREQUAL "src/render/rt64_render_target.h")
        jfg_vi_coverage_replace([==[#include "rt64_render_worker.h"]==] [==[#include "rt64_render_worker.h"
#include "rt64_coverage_estimate.h"]==])
        jfg_vi_coverage_replace([==[        bool usesHDR = false;]==] [==[        bool usesHDR = false;
        CoverageEstimate coverageEstimate;]==])
    elseif(relative STREQUAL "src/render/rt64_shader_library.h")
        jfg_vi_coverage_replace([==[        ShaderRecord boxFilter;]==] [==[        ShaderRecord boxFilter;
        ShaderRecord coverageEstimate;]==])
    elseif(relative STREQUAL "src/render/rt64_shader_library.cpp")
        foreach(prefix_format IN ITEMS "#include|spirv" "#   include|dxil" "#   include|metal")
            jfg_vi_coverage_split_pair("${prefix_format}" prefix format)
            jfg_vi_coverage_replace("${prefix} \"shaders/BoxFilterCS.hlsl.${format}.h\""
                "${prefix} \"shaders/BoxFilterCS.hlsl.${format}.h\"\n${prefix} \"shaders/CoverageEstimateCS.hlsl.${format}.h\"")
        endforeach()
        jfg_vi_coverage_replace([==[#include "shared/rt64_video_interface.h"]==] [==[#include "shared/rt64_video_interface.h"
#include "shared/rt64_coverage_estimate_params.h"]==])
        jfg_vi_coverage_replace([==[            boxFilter.pipeline = device->createComputePipeline(pipelineDesc);
        }]==] [==[            boxFilter.pipeline = device->createComputePipeline(pipelineDesc);
        }

        // Video core coverage estimate.
        {
            CoverageEstimateDescriptorSet descriptorSet;
            layoutBuilder.begin();
            layoutBuilder.addPushConstant(0, 0, sizeof(interop::CoverageEstimateCB), RenderShaderStageFlag::COMPUTE);
            layoutBuilder.addDescriptorSet(descriptorSet);
            layoutBuilder.end();
            coverageEstimate.pipelineLayout = layoutBuilder.create(device);

            std::unique_ptr<RenderShader> computeShader = device->createShader(CREATE_SHADER_INPUTS(CoverageEstimateCSBlobDXIL, CoverageEstimateCSBlobSPIRV, CoverageEstimateCSBlobMSL, "CSMain", shaderFormat));
            RenderComputePipelineDesc pipelineDesc(coverageEstimate.pipelineLayout.get(), computeShader.get(), 8, 8, 1);
            coverageEstimate.pipeline = device->createComputePipeline(pipelineDesc);
        }]==])
    elseif(relative STREQUAL "CMakeLists.txt")
        jfg_vi_coverage_replace([==[build_compute_shader(rt64 "src/shaders/BoxFilterCS.hlsl")]==] [==[build_compute_shader(rt64 "src/shaders/BoxFilterCS.hlsl")
build_compute_shader(rt64 "src/shaders/CoverageEstimateCS.hlsl")]==])
    elseif(relative STREQUAL "src/shared/rt64_video_interface.h")
        jfg_vi_coverage_replace([==[        uint framebufferHeight;]==] [==[        uint framebufferHeight;
        // VI pre-pass with a coverage estimate: the input holds the front
        // colour and coverage (coverageInput 1), VI anti-aliasing of partially
        // covered pixels (AA modes 0 and 1) and the divot filter (0 or 1).
        uint coverageInput;
        uint viAntiAliasing;
        uint divot;]==])
    elseif(relative STREQUAL "src/shaders/VideoInterfacePS.hlsl")
        jfg_vi_coverage_replace([==[int3 ViFilteredPixel(int2 pixel) {
    const int3 centre = ViFetchPixel(pixel);
    const int3 centre5 = centre >> 3;]==] [==[// Coverage as the RDP stores it (covered sub-samples minus one). Without a
// coverage estimate, and in AA modes 2 and 3, every pixel is fully covered.
static const uint ViFullCoverage = 7;

uint ViFetchCoverage(int2 pixel) {
    if ((gConstants.coverageInput == 0) || (gConstants.viAntiAliasing == 0)) {
        return ViFullCoverage;
    }

    const int2 framebufferSize = int2(gConstants.framebufferWidth, gConstants.framebufferHeight);
    pixel = clamp(pixel, int2(0, 0), framebufferSize - 1);
    const float2 texelsPerPixel = gConstants.videoResolution / float2(framebufferSize);
    const int2 lastTexel = int2(gConstants.textureResolution) - 1;
    const int2 texel = min(int2((float2(pixel) + 0.5f) * texelsPerPixel), lastTexel);
    return uint(round(saturate(gInput.Load(int3(texel, 0)).a) * 255.0f));
}

// The fully covered neighbours VI anti-aliasing reconstructs the background
// from.
static const int2 ViAntiAliasNeighbours[6] = { int2(-1, -1), int2(1, -1), int2(-2, 0), int2(2, 0), int2(-1, 1), int2(1, 1) };

// VI anti-aliasing of a partially covered pixel (n64_video/vi_anti_aliasing.hpp):
// per channel, the penultimate maximum and minimum of the centre (counted
// twice) and the fully covered neighbours stand for the background, and the
// pixel moves toward it by its missing coverage.
int3 ViAntiAliasedPixel(int2 pixel, int3 centre, uint coverage) {
    int3 largest = centre;
    int3 secondLargest = centre;
    int3 smallest = centre;
    int3 secondSmallest = centre;
    [unroll] for (int i = 0; i < 6; i++) {
        const int2 neighbour = pixel + ViAntiAliasNeighbours[i];
        if (ViFetchCoverage(neighbour) == ViFullCoverage) {
            const int3 value = ViFetchPixel(neighbour);
            secondLargest = select(value > largest, largest, max(secondLargest, value));
            largest = max(largest, value);
            secondSmallest = select(value < smallest, smallest, min(secondSmallest, value));
            smallest = min(smallest, value);
        }
    }

    const int3 backgroundOffset = secondSmallest + secondLargest - 2 * centre;
    return centre + ((backgroundOffset * int(ViFullCoverage - coverage) + 4) >> 3);
}

int3 ViFilteredPixel(int2 pixel) {
    const int3 centre = ViFetchPixel(pixel);
    const uint coverage = ViFetchCoverage(pixel);
    if (coverage < ViFullCoverage) {
        return ViAntiAliasedPixel(pixel, centre, coverage);
    }

    const int3 centre5 = centre >> 3;]==])
        jfg_vi_coverage_replace([==[float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    // The sample's position in framebuffer pixels]==] [==[// The divot filter: where the pixel or its left or right neighbour is
// partially covered, each channel becomes the median of the three filtered
// pixels.
int3 ViDivotPixel(int2 pixel) {
    const int3 centre = ViFilteredPixel(pixel);
    if ((gConstants.coverageInput == 0) || (gConstants.divot == 0)) {
        return centre;
    }

    const int2 left = pixel + int2(-1, 0);
    const int2 right = pixel + int2(1, 0);
    if ((ViFetchCoverage(left) & ViFetchCoverage(pixel) & ViFetchCoverage(right)) == ViFullCoverage) {
        return centre;
    }

    const int3 leftColor = ViFilteredPixel(left);
    const int3 rightColor = ViFilteredPixel(right);
    return max(min(leftColor, centre), min(max(leftColor, centre), rightColor));
}

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    // The sample's position in framebuffer pixels]==])
        jfg_vi_coverage_replace([==[    int3 color = ViFilteredPixel(pixel);]==] [==[    int3 color = ViDivotPixel(pixel);]==])
        jfg_vi_coverage_replace([==[        const int3 next = ViFilteredPixel(pixel + int2(1, 0));]==] [==[        const int3 next = ViDivotPixel(pixel + int2(1, 0));]==])
    elseif(relative STREQUAL "src/render/rt64_vi_renderer.h")
        jfg_vi_coverage_replace([==[            const RenderFramebuffer *framebuffer = nullptr;]==] [==[            const RenderFramebuffer *framebuffer = nullptr;
            // The frame's coverage estimate (front colour, coverage in alpha).
            // When set, the pre-pass reads it instead of `texture`.
            RenderTexture *coverageTexture = nullptr;]==])
    elseif(relative STREQUAL "src/render/rt64_vi_renderer.cpp")
        jfg_vi_coverage_replace([==[        prepass.descriptorSet->setTexture(prepass.descriptorSet->gInput, p.texture, RenderTextureLayout::SHADER_READ);]==] [==[        // With a coverage estimate the pre-pass reads it instead of the
        // averaged frame: each pixel's front colour and coverage.
        const RenderTexture *input = p.texture;
        if (p.coverageTexture != nullptr) {
            p.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(p.coverageTexture, RenderTextureLayout::SHADER_READ));
            input = p.coverageTexture;
        }

        prepass.descriptorSet->setTexture(prepass.descriptorSet->gInput, input, RenderTextureLayout::SHADER_READ);]==])
        jfg_vi_coverage_replace([==[        pushConstants.framebufferHeight = height;]==] [==[        pushConstants.framebufferHeight = height;
        pushConstants.coverageInput = (p.coverageTexture != nullptr) ? 1 : 0;
        pushConstants.viAntiAliasing = filtering.anti_aliasing ? 1 : 0;
        pushConstants.divot = filtering.divot ? 1 : 0;]==])
    elseif(relative STREQUAL "src/hle/rt64_present_queue.cpp")
        jfg_vi_coverage_replace([==[                        renderParams.textureHeight = colorTarget->height;]==] [==[                        renderParams.textureHeight = colorTarget->height;
                        renderParams.coverageTexture = coverageEstimateTexture(colorTarget);]==])
    elseif(relative STREQUAL "src/render/rt64_render_target.cpp")
        jfg_vi_coverage_replace([==[        fbWriteDescSet.reset();
    }]==] [==[        fbWriteDescSet.reset();
        coverageEstimate.release();
    }]==])
        jfg_vi_coverage_replace([==[        resolvedTextureDirty = true;]==] [==[        resolvedTextureDirty = true;
        coverageEstimate.valid = false;]==])
    elseif(relative STREQUAL "src/render/rt64_framebuffer_renderer.cpp")
        jfg_vi_coverage_replace([==[#include "rt64_framebuffer_renderer.h"]==] [==[#include "rt64_framebuffer_renderer.h"

#include "n64_video/video_options.hpp"]==])
        jfg_vi_coverage_replace([==[        frameParams.ditherNoiseStrength = ditherNoiseStrength;]==] [==[        frameParams.ditherNoiseStrength = ditherNoiseStrength;
        frameParams.coverageRecord = n64_video::estimates_coverage(n64_video::active_options()) ? 1 : 0;]==])
    elseif(relative STREQUAL "src/render/rt64_raster_shader.cpp")
        jfg_vi_coverage_replace([==[#include "rt64_raster_shader.h"]==] [==[#include "rt64_raster_shader.h"

#include "n64_video/coverage_estimate.hpp"
#include "n64_video/video_options.hpp"]==])
        jfg_vi_coverage_replace([==[            auto &locations = multisampling.sampleLocations;
            switch (sampleCount) {]==] [==[            auto &locations = multisampling.sampleLocations;

            // Video core coverage estimate: the RDP's own sample positions.
            const n64_video::MsaaSamplePattern rdpPattern = n64_video::estimates_coverage(n64_video::active_options()) ?
                n64_video::msaa_sample_pattern(uint32_t(sampleCount)) : n64_video::MsaaSamplePattern();
            if (rdpPattern.count == uint32_t(sampleCount)) {
                for (uint32_t s = 0; s < rdpPattern.count; s++) {
                    locations[s] = { int8_t(rdpPattern.positions[s].x_sixteenths), int8_t(rdpPattern.positions[s].y_sixteenths) };
                }

                return multisampling;
            }

            switch (sampleCount) {]==])
    elseif(relative STREQUAL "src/hle/rt64_workload_queue.cpp")
        jfg_vi_coverage_replace([==[            framebufferRenderer->recordSetup(]==] [==[            beginCoverageEstimateFrame();
            framebufferRenderer->recordSetup(]==])
        jfg_vi_coverage_replace([==[                    // Do the resolve if using MSAA while target override is active]==] [==[                    // Video core: estimate the pair's coverage for the VI pass.
                    estimatePairCoverage(ext.workloadGraphicsWorker, ext.shaderLibrary, targetManager.multisampling, colorTarget, depthTarget);

                    // Do the resolve if using MSAA while target override is active]==])
        jfg_vi_coverage_replace([==[                        overrideTarget->resolveFromTarget(ext.workloadGraphicsWorker, colorTarget, ext.shaderLibrary);]==] [==[                        overrideTarget->resolveFromTarget(ext.workloadGraphicsWorker, colorTarget, ext.shaderLibrary);
                        copyCoverageEstimate(ext.workloadGraphicsWorker, overrideTarget, colorTarget);]==])
    else()
        message(FATAL_ERROR "No VI coverage RT64 patch for ${relative}")
    endif()
    set(contents "${contents}" PARENT_SCOPE)
endfunction()

# Patches the files RT64's own build reads (shaders, headers, its
# CMakeLists.txt) in the build-private RT64 source view, on top of the
# versions earlier patches wrote there (the VI-pass and fog patches), and
# adds the coverage patch's new RT64 files. Call it after
# jfg_n64_video_patch_source_view.
function(jfg_vi_coverage_patch_source_view original_root source_view)
    set(patch_files "${PROJECT_SOURCE_DIR}/src/renderer/n64_video/rt64_patch")
    set(new_files
        "CoverageEstimateCS.hlsl|src/shaders/CoverageEstimateCS.hlsl"
        "CoverageRecord.hlsli|src/shaders/CoverageRecord.hlsli"
        "rt64_coverage_estimate_params.h|src/shared/rt64_coverage_estimate_params.h"
        "rt64_coverage_estimate.h|src/render/rt64_coverage_estimate.h")
    foreach(new_file IN LISTS new_files)
        jfg_vi_coverage_split_pair("${new_file}" name relative)
        # The copy happens at configure time, so editing a source reconfigures.
        set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch_files}/${name}")
        file(READ "${patch_files}/${name}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        jfg_vi_coverage_write("${source_view}/${relative}" "${contents}")
    endforeach()

    set(inputs
        "src/shared/rt64_frame_params.h|dee91d3ff78177821d02d4a27cc43606a58f9a54d7896c5d17b7762a9b29ff3c"
        "src/shaders/RasterPS.hlsl|957b283411550457573421015580f84ca750c68a050981dbc1a4fe2634507820"
        "src/render/rt64_render_target.h|5c234505140abe973c98c3d06c0558f6f66f6c67266ecac362c250137b3290d1"
        "src/render/rt64_shader_library.h|582214e47482b6f97b57b451a0051ff10711372c29b53e2dc870c398d8c1e075"
        "src/render/rt64_shader_library.cpp|72421375b232d5449f2f90f153c8452420084f43809c3234bc9727eddfde6ffd"
        "CMakeLists.txt|4d6cb49be103302c019405794995aac6b526b759f257f4786a7a2a00d4de3fa2"
        "src/shared/rt64_video_interface.h|5d97c854e5d4d0f1f40c5cf06815265baf9218982bcdf698d2b9b9fcf48fdb68"
        "src/shaders/VideoInterfacePS.hlsl|f4990ef5edfbdece9c89938a0c4951b3a285f99be9e6d92a415433c0e227f961"
        "src/render/rt64_vi_renderer.h|b03a6ea437eefc7a86f163ab72fa04b800ada5b9aa4fc7a15cf60a01378a3bc1")
    foreach(input IN LISTS inputs)
        jfg_vi_coverage_split_pair("${input}" relative expected)
        jfg_vi_coverage_check_input("${original_root}" "${relative}" "${expected}")
        file(READ "${source_view}/${relative}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        jfg_vi_coverage_patch_contents("${relative}")
        jfg_vi_coverage_write("${source_view}/${relative}" "${contents}")
    endforeach()
endfunction()

# Patches RT64 translation units in the rt64 target, on top of whatever
# version of each unit the target compiles now, and adds the coverage
# estimate's own unit. Call it after jfg_n64_video_patch_rt64_target, which
# also puts the video core on the rt64 target's include path.
function(jfg_vi_coverage_patch_rt64_target original_root)
    set(inputs
        "src/render/rt64_render_target.cpp|0dee0d19a4be22c4fa34b7ef793e3d4f75a07ab0d31252f9a23a2a0355edb41d"
        "src/render/rt64_framebuffer_renderer.cpp|73870c23b2340611b930b668d52aec9e687fd74b7da310962b7b1ef35dee015d"
        "src/render/rt64_raster_shader.cpp|5e5de4bbd8a22192e857c277bae5af033b8606a6464aa73863d2efe0fd0a0d4d"
        "src/hle/rt64_workload_queue.cpp|6e53030e8899c42347b51e67879fcb9679ad487ac067c192612c432d85bca4a4"
        "src/render/rt64_vi_renderer.cpp|0159b441fd4872ef30a49d7603f4570d8858084004c20dd6646f51039241fa7b"
        "src/hle/rt64_present_queue.cpp|023dc3f4bc74bf6863e37ad1781fb32eaa543a7713bd8b9a3757b2262a1a1e52")
    foreach(input IN LISTS inputs)
        jfg_vi_coverage_split_pair("${input}" relative expected)
        jfg_vi_coverage_check_input("${original_root}" "${relative}" "${expected}")
        get_filename_component(basename "${relative}" NAME)
        get_target_property(sources rt64 SOURCES)
        set(patched_sources)
        set(compiled_unit "")
        foreach(unit IN LISTS sources)
            get_filename_component(unit_name "${unit}" NAME)
            if(unit_name STREQUAL basename)
                set(compiled_unit "${unit}")
            else()
                list(APPEND patched_sources "${unit}")
            endif()
        endforeach()
        if(compiled_unit STREQUAL "")
            message(FATAL_ERROR "RT64 target is missing ${relative}")
        endif()
        if(NOT IS_ABSOLUTE "${compiled_unit}")
            get_target_property(rt64_source_dir rt64 SOURCE_DIR)
            set(compiled_unit "${rt64_source_dir}/${compiled_unit}")
        endif()
        file(READ "${compiled_unit}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        jfg_vi_coverage_patch_contents("${relative}")
        set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/rt64-vi-coverage/${basename}")
        jfg_vi_coverage_write("${generated}" "${contents}")
        list(APPEND patched_sources "${generated}")
        set_property(TARGET rt64 PROPERTY SOURCES "${patched_sources}")
    endforeach()
    target_sources(rt64 PRIVATE
        "${PROJECT_SOURCE_DIR}/src/renderer/n64_video/rt64_patch/rt64_coverage_estimate.cpp")
endfunction()
