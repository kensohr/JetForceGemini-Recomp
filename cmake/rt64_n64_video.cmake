# Video core (src/renderer/n64_video) RT64 patch: a native-resolution VI
# pre-pass that reduces a 16-bit frame to the console's dithered 16-bit
# colour, then runs the VI dither filter and resamples each row of a 16- or
# 32-bit frame to the active scanline width, before RT64's VI pass applies
# gamma and scales it to the window.
#
# Every patched file is checked against the pinned RT64 input (line endings
# normalised), so a changed RT64 file fails the build instead of being patched
# blindly. The code the patch puts into RT64 stays small and free of project
# names, so another project's RT64 can take the same patch; only these CMake
# helpers carry this project's jfg_ prefix.

# Replaces one patch site in `contents`; a missing site fails the build.
function(jfg_n64_video_replace before after)
    string(FIND "${contents}" "${before}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "Missing video core RT64 patch site: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" contents "${contents}")
    set(contents "${contents}" PARENT_SCOPE)
endfunction()

# Splits an "a|b" pair from an input list into two variables.
function(jfg_n64_video_split_pair pair first_variable second_variable)
    string(REPLACE "|" ";" fields "${pair}")
    list(GET fields 0 first)
    list(GET fields 1 second)
    set(${first_variable} "${first}" PARENT_SCOPE)
    set(${second_variable} "${second}" PARENT_SCOPE)
endfunction()

# Fails the build unless `relative` in the pinned RT64 checkout still has the
# audited content. The hash is of the LF-normalised file, so an LF and a CRLF
# checkout both match it (cmake/rt64_vi_presentation.cmake hashes raw bytes and
# so lists the CRLF hash of rt64_vi_renderer.cpp separately).
function(jfg_n64_video_check_input original_root relative expected)
    file(READ "${original_root}/${relative}" pinned)
    string(REPLACE "\r\n" "\n" pinned "${pinned}")
    string(SHA256 digest "${pinned}")
    if(NOT digest STREQUAL expected)
        message(FATAL_ERROR "Pinned video core RT64 input changed: ${relative}")
    endif()
endfunction()

# Applies the patches for `relative` to `contents`.
function(jfg_n64_video_patch_contents relative)
    if(relative STREQUAL "src/shaders/VideoInterfacePS.hlsl")
        jfg_n64_video_replace([==[float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
#ifdef PIXEL_ANTIALIASING]==] [==[#ifdef VI_PREPASS
// Native-resolution VI pre-pass: one invocation per output sample of each
// framebuffer row. It reduces a 16-bit frame to the RDP's 16-bit colour, runs
// the VI dither filter and resamples the row to the active scanline width.

// RDP colour-dither matrices, indexed by ((y & 3) << 2) | (x & 3). Same
// values as RT64's DitherPatternMagicSquare and DitherPatternBayer.
static const uint ViDitherMagicSquare[16] = { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 };
static const uint ViDitherBayer[16] = { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 };

uint ViDitherValue(int2 pixel) {
    const uint index = ((uint(pixel.y) & 3) << 2) | (uint(pixel.x) & 3);
    return (gConstants.ditherPattern == 1) ? ViDitherBayer[index] : ViDitherMagicSquare[index];
}

// The RDP's dithered 16-bit colour write: a channel rounds up to the next
// 5-bit step when its low 3 bits exceed the dither value (0-7), clamped at 31,
// else keeps its top 5 bits. (c + 7 - d) >> 3 carries into the top 5 bits
// exactly when (c & 7) > d.
int3 ReduceToRGB5(float3 color, uint dither) {
    const uint3 channel8 = uint3(round(saturate(color) * 255.0f));
    return int3(min((channel8 + 7u - dither) >> 3, 31u));
}

// A framebuffer pixel as the VI reads it, in 8-bit channels, from the rendered
// frame's texel at the pixel's centre (the same pixel at 1x). A 16-bit
// framebuffer's 5-bit channels read with the low 3 bits clear. Pixels outside
// the framebuffer repeat its edge.
int3 ViFetchPixel(int2 pixel) {
    const int2 framebufferSize = int2(gConstants.framebufferWidth, gConstants.framebufferHeight);
    pixel = clamp(pixel, int2(0, 0), framebufferSize - 1);
    const float2 texelsPerPixel = gConstants.videoResolution / float2(framebufferSize);
    const int2 lastTexel = int2(gConstants.textureResolution) - 1;
    const int2 texel = min(int2((float2(pixel) + 0.5f) * texelsPerPixel), lastTexel);
    const float3 color = gInput.Load(int3(texel, 0)).rgb;
    if (gConstants.colourReduction != 0) {
        return ReduceToRGB5(color, ViDitherValue(pixel)) << 3;
    }

    return int3(round(saturate(color) * 255.0f));
}

// The VI's dither filter restores some of the low bits a 16-bit framebuffer
// dropped: each channel moves one step toward each of the 8 neighbours whose
// top 5 bits differ from its own. It does the same on a 32-bit framebuffer.
int3 ViFilteredPixel(int2 pixel) {
    const int3 centre = ViFetchPixel(pixel);
    const int3 centre5 = centre >> 3;
    int3 color = centre;
    if (gConstants.ditherFilter != 0) {
        [unroll] for (int dy = -1; dy <= 1; dy++) {
            [unroll] for (int dx = -1; dx <= 1; dx++) {
                if ((dx != 0) || (dy != 0)) {
                    color += sign((ViFetchPixel(pixel + int2(dx, dy)) >> 3) - centre5);
                }
            }
        }
    }

    return color;
}

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    // The sample's position in framebuffer pixels, 2.10 fixed point. The VI
    // interpolates with the top 5 bits of the fraction.
    const uint2 outputPixel = uint2(pos.xy);
    const uint position = gConstants.xOffset + outputPixel.x * gConstants.xStep;
    const int2 pixel = int2(position >> 10, outputPixel.y);
    int3 color = ViFilteredPixel(pixel);
    if (gConstants.resampling == 0) {
        const int fraction = int((position >> 5) & 31);
        const int3 next = ViFilteredPixel(pixel + int2(1, 0));
        color += ((next - color) * fraction + 16) >> 5;
    }

    return float4(float3(color) / 255.0f, 1.0f);
}
#else
float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
#ifdef PIXEL_ANTIALIASING]==])
        string(APPEND contents "\n#endif // VI_PREPASS\n")
    elseif(relative STREQUAL "src/shared/rt64_video_interface.h")
        jfg_n64_video_replace([==[        float gamma;]==] [==[        float gamma;
        // VI pre-pass: 16-bit colour reduction (0 or 1), RDP colour-dither
        // pattern (0 magic square, 1 Bayer), VI dither filter (0 or 1),
        // resampling (0 interpolate, 1 replicate), X step and offset (2.10
        // fixed point) and the framebuffer size.
        uint colourReduction;
        uint ditherPattern;
        uint ditherFilter;
        uint resampling;
        uint xStep;
        uint xOffset;
        uint framebufferWidth;
        uint framebufferHeight;]==])
    elseif(relative STREQUAL "CMakeLists.txt")
        jfg_n64_video_replace([==[build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPixel.hlsl" "-D PIXEL_ANTIALIASING")]==] [==[build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPixel.hlsl" "-D PIXEL_ANTIALIASING")
build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPrepass.hlsl" "-D VI_PREPASS")]==])
    elseif(relative STREQUAL "src/render/rt64_shader_library.h")
        jfg_n64_video_replace([==[        ShaderRecord videoInterfacePixel;]==] [==[        ShaderRecord videoInterfacePixel;
        ShaderRecord videoInterfacePrepass;]==])
    elseif(relative STREQUAL "src/render/rt64_shader_library.cpp")
        foreach(prefix_format IN ITEMS "#include|spirv" "#   include|dxil" "#   include|metal")
            jfg_n64_video_split_pair("${prefix_format}" prefix format)
            jfg_n64_video_replace("${prefix} \"shaders/VideoInterfacePSPixel.hlsl.${format}.h\""
                "${prefix} \"shaders/VideoInterfacePSPixel.hlsl.${format}.h\"\n${prefix} \"shaders/VideoInterfacePSPrepass.hlsl.${format}.h\"")
        endforeach()
        jfg_n64_video_replace([==[            videoInterfacePixel.pipeline = device->createGraphicsPipeline(pipelineDesc);]==] [==[            videoInterfacePixel.pipeline = device->createGraphicsPipeline(pipelineDesc);

            // VI pre-pass: reads the frame with Load, so the nearest sampler
            // only fixes the descriptor layout.
            std::unique_ptr<RenderShader> prepassShader = device->createShader(CREATE_SHADER_INPUTS(VideoInterfacePSPrepassBlobDXIL, VideoInterfacePSPrepassBlobSPIRV, VideoInterfacePSPrepassBlobMSL, "PSMain", shaderFormat));
            layoutBuilder.begin();
            layoutBuilder.addPushConstant(0, 0, sizeof(interop::VideoInterfaceCB), RenderShaderStageFlag::PIXEL);
            layoutBuilder.addDescriptorSet(nearestDescriptorSet);
            layoutBuilder.end();
            videoInterfacePrepass.pipelineLayout = layoutBuilder.create(device);
            pipelineDesc.pixelShader = prepassShader.get();
            pipelineDesc.pipelineLayout = videoInterfacePrepass.pipelineLayout.get();
            videoInterfacePrepass.pipeline = device->createGraphicsPipeline(pipelineDesc);]==])
    elseif(relative STREQUAL "src/render/rt64_vi_renderer.h")
        jfg_n64_video_replace([==[        const RenderSampler *descriptorSetSampler = nullptr;]==] [==[        const RenderSampler *descriptorSetSampler = nullptr;

        // The VI pre-pass's native-resolution output and its input binding.
        struct Prepass {
            std::unique_ptr<VideoInterfaceDescriptorSet> descriptorSet;
            std::unique_ptr<RenderTexture> texture;
            std::unique_ptr<RenderFramebuffer> framebuffer;
            uint32_t width = 0;
            uint32_t height = 0;
        };
        Prepass prepass;]==])
        jfg_n64_video_replace([==[            bool removeBlackBorders = false;]==] [==[            bool removeBlackBorders = false;
            // The framebuffer the VI pass draws into, rebound after the
            // pre-pass. Without it the pre-pass doesn't run.
            const RenderFramebuffer *framebuffer = nullptr;]==])
        jfg_n64_video_replace([==[        void render(const RenderParams &p);]==] [==[        void render(const RenderParams &p);
        bool renderPrepass(const RenderParams &p);]==])
    elseif(relative STREQUAL "src/render/rt64_vi_renderer.cpp")
        jfg_n64_video_replace([==[#include "shared/rt64_video_interface.h"]==] [==[#include "shared/rt64_video_interface.h"

#include "n64_video/colour_reduction.hpp"
#include "n64_video/vi_filtering.hpp"]==])
        jfg_n64_video_replace([==[    void VIRenderer::render(const RenderParams &p) {]==] [==[    void VIRenderer::render(const RenderParams &frameParams) {
        // When the pre-pass runs, its native-resolution output replaces the
        // frame this pass scales to the window.
        RenderParams p = frameParams;
        hlslpp::float2 videoResolution = computeHDSize(hlslpp::float2(p.vi->fbSize()), p.resolutionScale, p.downsamplingScale);
        if (renderPrepass(frameParams)) {
            p.texture = prepass.texture.get();
            p.textureWidth = prepass.width;
            p.textureHeight = prepass.height;
            videoResolution = hlslpp::float2(float(prepass.width), float(prepass.height));
        }
]==])
        jfg_n64_video_replace([==[        interop::VideoInterfaceCB pushConstants;
        pushConstants.videoResolution = computeHDSize(hlslpp::float2(p.vi->fbSize()), p.resolutionScale, p.downsamplingScale);]==] [==[        interop::VideoInterfaceCB pushConstants = {};
        pushConstants.videoResolution = videoResolution;]==])
        jfg_n64_video_replace([==[    void VIRenderer::getViewportAndScissor(]==] [==[    // Draws the frame into prepass.texture, one row per framebuffer row and
    // one column per sample of the active scanline: reduced to the console's
    // 16-bit colour (16-bit framebuffers only), dither-filtered and resampled
    // as the video core decides from its options and the VI registers.
    // Returns false, drawing nothing, when VI filtering is off or the VI shows
    // nothing.
    bool VIRenderer::renderPrepass(const RenderParams &p) {
        const n64_video::VideoOptions &options = n64_video::active_options();
        const uint32_t viControl = p.vi->status.word;
        if (!n64_video::vi_filtering_enabled(options, viControl) || (p.framebuffer == nullptr)) {
            return false;
        }

        const n64_video::ColourReduction reduction = n64_video::colour_reduction(options, viControl);
        const n64_video::ViFiltering filtering = n64_video::vi_filtering({ viControl, p.vi->hRegion.word, p.vi->xTransform.word });
        const hlslpp::uint2 framebufferSize = p.vi->fbSize();
        const uint32_t framebufferWidth = framebufferSize.x;
        const uint32_t width = filtering.active_width;
        const uint32_t height = framebufferSize.y;
        const bool hasFrame = (framebufferWidth > 0) && (height > 0) && (width > 0) && (filtering.x_step > 0);
        if (!hasFrame) {
            return false;
        }

        // Replacing the texture frees it while this frame records. That's
        // safe for the same reason RT64 replaces descriptorSet in render()
        // and resizes render targets on this thread: the present queue waits
        // for each frame's command list before it records the next, so no
        // submitted work still uses the old texture.
        if ((prepass.texture == nullptr) || (prepass.width != width) || (prepass.height != height)) {
            prepass.framebuffer.reset();
            prepass.texture = p.device->createTexture(RenderTextureDesc::ColorTarget(width, height, RenderFormat::B8G8R8A8_UNORM));
            const RenderTexture *colorAttachment = prepass.texture.get();
            prepass.framebuffer = p.device->createFramebuffer(RenderFramebufferDesc(&colorAttachment, 1));
            prepass.width = width;
            prepass.height = height;
        }

        if (prepass.descriptorSet == nullptr) {
            prepass.descriptorSet = std::make_unique<VideoInterfaceDescriptorSet>(p.shaderLibrary->samplerLibrary.nearest.borderBorder.get(), p.device);
        }

        prepass.descriptorSet->setTexture(prepass.descriptorSet->gInput, p.texture, RenderTextureLayout::SHADER_READ);

        // No gamma: the VI pass after the pre-pass applies it.
        interop::VideoInterfaceCB pushConstants = {};
        pushConstants.videoResolution = computeHDSize(hlslpp::float2(framebufferSize), p.resolutionScale, p.downsamplingScale);
        pushConstants.textureResolution = { float(p.textureWidth), float(p.textureHeight) };
        pushConstants.colourReduction = reduction.enabled ? 1 : 0;
        pushConstants.ditherPattern = uint32_t(reduction.pattern);
        pushConstants.ditherFilter = filtering.dither_filter ? 1 : 0;
        pushConstants.resampling = uint32_t(filtering.resampling);
        pushConstants.xStep = filtering.x_step;
        pushConstants.xOffset = filtering.x_offset;
        pushConstants.framebufferWidth = framebufferWidth;
        pushConstants.framebufferHeight = height;

        const ShaderRecord &shader = p.shaderLibrary->videoInterfacePrepass;
        p.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(prepass.texture.get(), RenderTextureLayout::COLOR_WRITE));
        p.commandList->setFramebuffer(prepass.framebuffer.get());
        p.commandList->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
        p.commandList->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));
        p.commandList->setPipeline(shader.pipeline.get());
        p.commandList->setGraphicsPipelineLayout(shader.pipelineLayout.get());
        p.commandList->setGraphicsDescriptorSet(prepass.descriptorSet->get(), 0);
        p.commandList->setGraphicsPushConstants(0, &pushConstants);
        p.commandList->setVertexBuffers(0, nullptr, 0, nullptr);
        p.commandList->drawInstanced(3, 1, 0, 0);
        p.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(prepass.texture.get(), RenderTextureLayout::SHADER_READ));
        p.commandList->setFramebuffer(p.framebuffer);
        return true;
    }

    void VIRenderer::getViewportAndScissor(]==])
    elseif(relative STREQUAL "src/hle/rt64_present_queue.cpp")
        jfg_n64_video_replace([==[                    renderParams.removeBlackBorders = removeBlackBorders;]==] [==[                    renderParams.removeBlackBorders = removeBlackBorders;
                    renderParams.framebuffer = swapChainFramebuffer;]==])
    else()
        message(FATAL_ERROR "No video core RT64 patch for ${relative}")
    endif()
    set(contents "${contents}" PARENT_SCOPE)
endfunction()

# Patches the files RT64's own build reads (shaders, headers, its
# CMakeLists.txt) in a build-private copy of the RT64 source, before
# add_subdirectory. `original_root` is the pinned checkout, `source_view` the
# copy RT64 is built from.
function(jfg_n64_video_patch_source_view original_root source_view)
    set(inputs
        "src/shaders/VideoInterfacePS.hlsl|f4990ef5edfbdece9c89938a0c4951b3a285f99be9e6d92a415433c0e227f961"
        "src/shared/rt64_video_interface.h|5d97c854e5d4d0f1f40c5cf06815265baf9218982bcdf698d2b9b9fcf48fdb68"
        "CMakeLists.txt|4d6cb49be103302c019405794995aac6b526b759f257f4786a7a2a00d4de3fa2"
        "src/render/rt64_shader_library.h|582214e47482b6f97b57b451a0051ff10711372c29b53e2dc870c398d8c1e075"
        "src/render/rt64_shader_library.cpp|72421375b232d5449f2f90f153c8452420084f43809c3234bc9727eddfde6ffd"
        "src/render/rt64_vi_renderer.h|b03a6ea437eefc7a86f163ab72fa04b800ada5b9aa4fc7a15cf60a01378a3bc1")
    foreach(input IN LISTS inputs)
        jfg_n64_video_split_pair("${input}" relative expected)
        jfg_n64_video_check_input("${original_root}" "${relative}" "${expected}")
        file(READ "${original_root}/${relative}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        jfg_n64_video_patch_contents("${relative}")
        set(destination "${source_view}/${relative}")
        set(existing "")
        if(EXISTS "${destination}")
            file(READ "${destination}" existing)
        endif()
        if(NOT existing STREQUAL contents)
            file(WRITE "${destination}" "${contents}")
        endif()
    endforeach()
endfunction()

# Patches RT64 translation units in the rt64 target, on top of whatever
# version of each unit the target compiles now (other patches may already
# have replaced it), and points the target at the patched copy. Call it after
# every other patch of these units. `core_include` is the folder that holds
# n64_video/.
function(jfg_n64_video_patch_rt64_target original_root core_include)
    set(inputs
        "src/render/rt64_vi_renderer.cpp|0159b441fd4872ef30a49d7603f4570d8858084004c20dd6646f51039241fa7b"
        "src/hle/rt64_present_queue.cpp|023dc3f4bc74bf6863e37ad1781fb32eaa543a7713bd8b9a3757b2262a1a1e52")
    foreach(input IN LISTS inputs)
        jfg_n64_video_split_pair("${input}" relative expected)
        jfg_n64_video_check_input("${original_root}" "${relative}" "${expected}")
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
        jfg_n64_video_patch_contents("${relative}")
        set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/rt64-n64-video/${basename}")
        file(WRITE "${generated}" "${contents}")
        list(APPEND patched_sources "${generated}")
        set_property(TARGET rt64 PROPERTY SOURCES "${patched_sources}")
    endforeach()
    target_include_directories(rt64 PRIVATE "${core_include}")
endfunction()
