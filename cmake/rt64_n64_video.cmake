# Video core (src/renderer/n64_video) RT64 patch: a native-resolution VI
# pre-pass that reduces each presented frame to the console's dithered 16-bit
# colour before RT64's VI pass applies gamma and scales it to the window.
#
# Every patched file is checked against the pinned RT64 input (line endings
# normalised), so a changed RT64 file fails the build instead of being patched
# blindly. The changes stay small and free of project names so another
# project's RT64 can take the same patch.

# Replaces one patch site in `contents`; a missing site fails the build.
function(jfg_n64_video_replace before after)
    string(FIND "${contents}" "${before}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "Missing video core RT64 patch site: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" contents "${contents}")
    set(contents "${contents}" PARENT_SCOPE)
endfunction()

# Fails the build unless `relative` in the pinned RT64 checkout still has the
# audited content.
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
// Native-resolution VI pre-pass: one invocation per framebuffer pixel.

// RDP colour-dither matrices, indexed by ((y & 3) << 2) | (x & 3). Same
// values as RT64's DitherPatternMagicSquare and DitherPatternBayer.
static const uint ViDitherMagicSquare[16] = { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 };
static const uint ViDitherBayer[16] = { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 };

uint ViDitherValue(uint2 pixel) {
    const uint index = ((pixel.y & 3) << 2) | (pixel.x & 3);
    return (gConstants.ditherPattern == 1) ? ViDitherBayer[index] : ViDitherMagicSquare[index];
}

// The RDP's 16-bit colour write: add the dither value to each 8-bit channel,
// clamp, keep the top 5 bits. Expanded back to 8 bits like RT64's
// RGBA16ToFloat4.
float3 ReduceToRGBA5551(float3 color, uint dither) {
    const uint3 channel8 = uint3(round(saturate(color) * 255.0f));
    const uint3 channel5 = min(channel8 + dither, 255u) >> 3;
    return float3((channel5 << 3) | (channel5 >> 2)) / 255.0f;
}

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    // The rendered frame's texel at this framebuffer pixel's centre. At 1x
    // they are the same pixel.
    const int2 lastTexel = int2(gConstants.textureResolution) - 1;
    const int2 texel = min(int2(uv * gConstants.videoResolution), lastTexel);
    float3 color = gInput.Load(int3(texel, 0)).rgb;
    if (gConstants.colourReduction != 0) {
        color = ReduceToRGBA5551(color, ViDitherValue(uint2(pos.xy)));
    }

    return float4(color, 1.0f);
}
#else
float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
#ifdef PIXEL_ANTIALIASING]==])
        string(APPEND contents "\n#endif // VI_PREPASS\n")
    elseif(relative STREQUAL "src/shared/rt64_video_interface.h")
        jfg_n64_video_replace([==[        float gamma;]==] [==[        float gamma;
        // VI pre-pass: reduce to dithered 16-bit colour (0 or 1), with the
        // RDP colour-dither pattern (0 magic square, 1 Bayer).
        uint colourReduction;
        uint ditherPattern;]==])
    elseif(relative STREQUAL "CMakeLists.txt")
        jfg_n64_video_replace([==[build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPixel.hlsl" "-D PIXEL_ANTIALIASING")]==] [==[build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPixel.hlsl" "-D PIXEL_ANTIALIASING")
build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPrepass.hlsl" "-D VI_PREPASS")]==])
    elseif(relative STREQUAL "src/render/rt64_shader_library.h")
        jfg_n64_video_replace([==[        ShaderRecord videoInterfacePixel;]==] [==[        ShaderRecord videoInterfacePixel;
        ShaderRecord videoInterfacePrepass;]==])
    elseif(relative STREQUAL "src/render/rt64_shader_library.cpp")
        foreach(prefix_format IN ITEMS "#include|spirv" "#   include|dxil" "#   include|metal")
            string(REPLACE "|" ";" prefix_format "${prefix_format}")
            list(GET prefix_format 0 prefix)
            list(GET prefix_format 1 format)
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
        std::unique_ptr<VideoInterfaceDescriptorSet> prepassDescriptorSet;
        std::unique_ptr<RenderTexture> prepassTexture;
        std::unique_ptr<RenderFramebuffer> prepassFramebuffer;
        uint32_t prepassWidth = 0;
        uint32_t prepassHeight = 0;]==])
        jfg_n64_video_replace([==[            bool removeBlackBorders = false;]==] [==[            bool removeBlackBorders = false;
            // The framebuffer the VI pass draws into, rebound after the
            // pre-pass. Without it the pre-pass doesn't run.
            const RenderFramebuffer *framebuffer = nullptr;]==])
        jfg_n64_video_replace([==[        void render(const RenderParams &p);]==] [==[        void render(const RenderParams &p);
        bool renderPrepass(const RenderParams &p);]==])
    elseif(relative STREQUAL "src/render/rt64_vi_renderer.cpp")
        jfg_n64_video_replace([==[#include "shared/rt64_video_interface.h"]==] [==[#include "shared/rt64_video_interface.h"

#include "n64_video/colour_reduction.hpp"]==])
        jfg_n64_video_replace([==[    void VIRenderer::render(const RenderParams &p) {]==] [==[    void VIRenderer::render(const RenderParams &frameParams) {
        // When the pre-pass runs, its native-resolution output replaces the
        // frame this pass scales to the window.
        RenderParams p = frameParams;
        hlslpp::float2 videoResolution = computeHDSize(hlslpp::float2(p.vi->fbSize()), p.resolutionScale, p.downsamplingScale);
        if (renderPrepass(frameParams)) {
            p.texture = prepassTexture.get();
            p.textureWidth = prepassWidth;
            p.textureHeight = prepassHeight;
            videoResolution = hlslpp::float2(float(prepassWidth), float(prepassHeight));
        }
]==])
        jfg_n64_video_replace([==[        pushConstants.videoResolution = computeHDSize(hlslpp::float2(p.vi->fbSize()), p.resolutionScale, p.downsamplingScale);]==] [==[        pushConstants.videoResolution = videoResolution;
        pushConstants.colourReduction = 0;
        pushConstants.ditherPattern = 0;]==])
        jfg_n64_video_replace([==[    void VIRenderer::getViewportAndScissor(]==] [==[    // Draws the frame at framebuffer resolution into prepassTexture, reduced
    // to the console's 16-bit colour as the video core decides from its
    // options and VI_CONTROL. Returns false, drawing nothing, when the
    // reduction is off.
    bool VIRenderer::renderPrepass(const RenderParams &p) {
        const n64_video::ColourReduction reduction = n64_video::colour_reduction(n64_video::active_options(), p.vi->status.word);
        const hlslpp::uint2 framebufferSize = p.vi->fbSize();
        const uint32_t width = framebufferSize.x;
        const uint32_t height = framebufferSize.y;
        if (!reduction.enabled || (p.framebuffer == nullptr) || (width == 0) || (height == 0)) {
            return false;
        }

        if ((prepassTexture == nullptr) || (prepassWidth != width) || (prepassHeight != height)) {
            prepassFramebuffer.reset();
            prepassTexture = p.device->createTexture(RenderTextureDesc::ColorTarget(width, height, RenderFormat::B8G8R8A8_UNORM));
            const RenderTexture *colorAttachment = prepassTexture.get();
            prepassFramebuffer = p.device->createFramebuffer(RenderFramebufferDesc(&colorAttachment, 1));
            prepassWidth = width;
            prepassHeight = height;
        }

        if (prepassDescriptorSet == nullptr) {
            prepassDescriptorSet = std::make_unique<VideoInterfaceDescriptorSet>(p.shaderLibrary->samplerLibrary.nearest.borderBorder.get(), p.device);
        }

        prepassDescriptorSet->setTexture(prepassDescriptorSet->gInput, p.texture, RenderTextureLayout::SHADER_READ);

        interop::VideoInterfaceCB pushConstants;
        pushConstants.videoResolution = computeHDSize(hlslpp::float2(framebufferSize), p.resolutionScale, p.downsamplingScale);
        pushConstants.textureResolution = { float(p.textureWidth), float(p.textureHeight) };
        pushConstants.gamma = p.vi->gamma();
        pushConstants.colourReduction = 1;
        pushConstants.ditherPattern = uint32_t(reduction.pattern);

        const ShaderRecord &shader = p.shaderLibrary->videoInterfacePrepass;
        p.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(prepassTexture.get(), RenderTextureLayout::COLOR_WRITE));
        p.commandList->setFramebuffer(prepassFramebuffer.get());
        p.commandList->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
        p.commandList->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));
        p.commandList->setPipeline(shader.pipeline.get());
        p.commandList->setGraphicsPipelineLayout(shader.pipelineLayout.get());
        p.commandList->setGraphicsDescriptorSet(prepassDescriptorSet->get(), 0);
        p.commandList->setGraphicsPushConstants(0, &pushConstants);
        p.commandList->setVertexBuffers(0, nullptr, 0, nullptr);
        p.commandList->drawInstanced(3, 1, 0, 0);
        p.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(prepassTexture.get(), RenderTextureLayout::SHADER_READ));
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
        string(REPLACE "|" ";" input "${input}")
        list(GET input 0 relative)
        list(GET input 1 expected)
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
        string(REPLACE "|" ";" input "${input}")
        list(GET input 0 relative)
        list(GET input 1 expected)
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
