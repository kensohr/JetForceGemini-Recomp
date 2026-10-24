# Video core (src/renderer/n64_video) RT64 patch: a native-resolution VI
# pre-pass that reduces a 16-bit frame to the console's dithered 16-bit
# colour, then runs the VI's anti-aliasing, dither filter and divot filter,
# resamples each row of a 16- or 32-bit frame to the active scanline width and
# adds the VI's gamma-dither noise, before RT64's VI pass applies gamma and
# scales it to the window.
#
# VI anti-aliasing and the divot filter need each pixel's RDP coverage, which
# RT64 doesn't produce, so the patch also reaches past the VI pass:
# - MSAA uses the RDP's sample positions (RasterShader);
# - the raster shader marks the samples of anti-aliased draws in the colour
#   target's alpha (CoverageRecord.hlsli), switched on by a frame parameter
#   for multisampled targets; RT64's own coverage value stays as it is;
# - resolves take RT64's raster path and take the mark off each sample before
#   averaging (TextureResolvePS), so RT64 reads back the same coverage bits
#   as without the estimate;
# - after each framebuffer pair the workload thread runs CoverageEstimateCS
#   and keeps the result with the colour target (rt64_coverage_estimate.*),
#   copied to the interpolation target it's resolved into;
# - the present queue hands the estimate to the VI pre-pass, which reads it
#   instead of the averaged frame when it exists.
#
# Every patched file is checked against the pinned RT64 input (line endings
# normalised) and each patch site must exist in the version being patched, so
# a changed RT64 file or an earlier patch that moved a site fails the build
# instead of being patched blindly. Each file is written once; no site
# replaces text this script inserted. New RT64 files come from
# src/renderer/n64_video/rt64_patch/. The code the patch puts into RT64 is
# free of project names, so another project's RT64 can take the same patch;
# only these CMake helpers carry this project's jfg_ prefix.

include("${CMAKE_CURRENT_LIST_DIR}/rt64_patch_helpers.cmake")

# Applies the patches for `relative` to `contents`.
function(jfg_n64_video_patch_contents relative)
    if(relative STREQUAL "src/shaders/VideoInterfacePS.hlsl")
        jfg_rt64_patch_replace([==[#include "shared/rt64_video_interface.h"]==] [==[#include "shared/rt64_video_interface.h"
#include "shared/rt64_coverage_layout.h"]==])
        jfg_rt64_patch_replace([==[float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
#ifdef PIXEL_ANTIALIASING]==] [==[#ifdef VI_PREPASS
// Native-resolution VI pre-pass: one invocation per output sample of each
// framebuffer row. It reduces a 16-bit frame to the RDP's 16-bit colour, runs
// VI anti-aliasing or the dither filter, then the divot filter, resamples
// the row to the active scanline width and adds gamma-dither noise.

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

// Coverage as the RDP stores it (covered sub-samples minus one). Without a
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
    return uint(round(saturate(gInput.Load(int3(texel, 0)).a) * EstimateCoverageScale));
}

// The fully covered neighbours VI anti-aliasing reconstructs the background
// from.
static const int ViAntiAliasNeighbourCount = 6;
static const int2 ViAntiAliasNeighbours[ViAntiAliasNeighbourCount] = { int2(-1, -1), int2(1, -1), int2(-2, 0), int2(2, 0), int2(-1, 1), int2(1, 1) };

// VI anti-aliasing of a partially covered pixel (n64_video/vi_anti_aliasing.hpp):
// per channel, the penultimate maximum and minimum of the centre (counted
// twice) and the fully covered neighbours stand for the background, and the
// pixel moves toward it by its missing coverage.
int3 ViAntiAliasedPixel(int2 pixel, int3 centre, uint coverage) {
    int3 largest = centre;
    int3 secondLargest = centre;
    int3 smallest = centre;
    int3 secondSmallest = centre;
    [unroll] for (int i = 0; i < ViAntiAliasNeighbourCount; i++) {
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
    // The pixel moves by its missing sub-samples (7 - coverage) out of the 8
    // coverage sub-samples: + 4 rounds the divide, >> 3 divides by 8.
    return centre + ((backgroundOffset * int(ViFullCoverage - coverage) + 4) >> 3);
}

// A partially covered pixel is anti-aliased. A fully covered one goes through
// the VI's dither filter, which restores some of the low bits a 16-bit
// framebuffer dropped: each channel moves one step toward each of the 8
// neighbours whose top 5 bits differ from its own. It does the same on a
// 32-bit framebuffer.
int3 ViFilteredPixel(int2 pixel) {
    const int3 centre = ViFetchPixel(pixel);
    const uint coverage = ViFetchCoverage(pixel);
    if (coverage < ViFullCoverage) {
        return ViAntiAliasedPixel(pixel, centre, coverage);
    }

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

// The divot filter: where the pixel or its left or right neighbour is
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

// Gamma-dither noise (n64_video/gamma_dither.hpp): lowbias32 chained through
// the frame counter, the row and the sample gives three independent bits,
// red, green and blue; each adds to its 8-bit channel, which stays at most
// 255.
uint ViGammaDitherHash(uint value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

int3 ViGammaDither(uint2 outputPixel, int3 color) {
    const uint bits = ViGammaDitherHash(outputPixel.x + ViGammaDitherHash(outputPixel.y + ViGammaDitherHash(gConstants.noiseFrame)));
    const int3 noise = int3(bits & 1u, (bits >> 1) & 1u, (bits >> 2) & 1u);
    return min(color + noise, 255);
}

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    // The sample's position in framebuffer pixels, 2.10 fixed point. The VI
    // interpolates with the top 5 bits of the fraction.
    const uint2 outputPixel = uint2(pos.xy);
    const uint position = gConstants.xOffset + outputPixel.x * gConstants.xStep;
    const int2 pixel = int2(position >> 10, outputPixel.y);
    int3 color = ViDivotPixel(pixel);
    if (gConstants.resampling == 0) {
        const int fraction = int((position >> 5) & 31);
        const int3 next = ViDivotPixel(pixel + int2(1, 0));
        color += ((next - color) * fraction + 16) >> 5;
    }

    if (gConstants.gammaDither != 0) {
        color = ViGammaDither(outputPixel, color);
    }

    return float4(float3(color) / 255.0f, 1.0f);
}
#else
float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
#ifdef PIXEL_ANTIALIASING]==])
        string(APPEND contents "\n#endif // VI_PREPASS\n")
    elseif(relative STREQUAL "src/shared/rt64_video_interface.h")
        jfg_rt64_patch_replace([==[        float gamma;]==] [==[        float gamma;
        // VI pre-pass: 16-bit colour reduction (0 or 1), RDP colour-dither
        // pattern (0 magic square, 1 Bayer), VI dither filter (0 or 1),
        // resampling (0 interpolate, 1 replicate), X step and offset (2.10
        // fixed point) and the framebuffer size. With a coverage estimate the
        // input holds the front colour and coverage (coverageInput 1), with VI
        // anti-aliasing of partially covered pixels (AA modes 0 and 1) and the
        // divot filter (0 or 1). Gamma-dither noise (0 or 1) uses the
        // pre-pass's frame counter.
        uint colourReduction;
        uint ditherPattern;
        uint ditherFilter;
        uint resampling;
        uint xStep;
        uint xOffset;
        uint framebufferWidth;
        uint framebufferHeight;
        uint coverageInput;
        uint viAntiAliasing;
        uint divot;
        uint gammaDither;
        uint noiseFrame;]==])
    elseif(relative STREQUAL "CMakeLists.txt")
        jfg_rt64_patch_replace([==[build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPixel.hlsl" "-D PIXEL_ANTIALIASING")]==] [==[build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPixel.hlsl" "-D PIXEL_ANTIALIASING")
build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPrepass.hlsl" "-D VI_PREPASS")]==])
        jfg_rt64_patch_replace([==[build_compute_shader(rt64 "src/shaders/BoxFilterCS.hlsl")]==] [==[build_compute_shader(rt64 "src/shaders/BoxFilterCS.hlsl")
build_compute_shader(rt64 "src/shaders/CoverageEstimateCS.hlsl")]==])
    elseif(relative STREQUAL "src/render/rt64_shader_library.h")
        jfg_rt64_patch_replace([==[        ShaderRecord videoInterfacePixel;]==] [==[        ShaderRecord videoInterfacePixel;
        ShaderRecord videoInterfacePrepass;]==])
        jfg_rt64_patch_replace([==[        ShaderRecord boxFilter;]==] [==[        ShaderRecord boxFilter;
        ShaderRecord coverageEstimate;]==])
    elseif(relative STREQUAL "src/render/rt64_shader_library.cpp")
        foreach(prefix_format IN ITEMS "#include|spirv" "#   include|dxil" "#   include|metal")
            jfg_rt64_patch_split_pair("${prefix_format}" prefix format)
            jfg_rt64_patch_replace("${prefix} \"shaders/VideoInterfacePSPixel.hlsl.${format}.h\""
                "${prefix} \"shaders/VideoInterfacePSPixel.hlsl.${format}.h\"\n${prefix} \"shaders/VideoInterfacePSPrepass.hlsl.${format}.h\"")
        endforeach()
        jfg_rt64_patch_replace([==[            videoInterfacePixel.pipeline = device->createGraphicsPipeline(pipelineDesc);]==] [==[            videoInterfacePixel.pipeline = device->createGraphicsPipeline(pipelineDesc);

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
        foreach(prefix_format IN ITEMS "#include|spirv" "#   include|dxil" "#   include|metal")
            jfg_rt64_patch_split_pair("${prefix_format}" prefix format)
            jfg_rt64_patch_replace("${prefix} \"shaders/BoxFilterCS.hlsl.${format}.h\""
                "${prefix} \"shaders/BoxFilterCS.hlsl.${format}.h\"\n${prefix} \"shaders/CoverageEstimateCS.hlsl.${format}.h\"")
        endforeach()
        jfg_rt64_patch_replace([==[#include "shared/rt64_video_interface.h"]==] [==[#include "shared/rt64_video_interface.h"
#include "shared/rt64_coverage_estimate_params.h"]==])
        jfg_rt64_patch_replace([==[            boxFilter.pipeline = device->createComputePipeline(pipelineDesc);
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
    elseif(relative STREQUAL "src/render/rt64_vi_renderer.h")
        jfg_rt64_patch_replace([==[        const RenderSampler *descriptorSetSampler = nullptr;]==] [==[        const RenderSampler *descriptorSetSampler = nullptr;

        // The VI pre-pass's native-resolution output and its input binding.
        struct Prepass {
            std::unique_ptr<VideoInterfaceDescriptorSet> descriptorSet;
            std::unique_ptr<RenderTexture> texture;
            std::unique_ptr<RenderFramebuffer> framebuffer;
            uint32_t width = 0;
            uint32_t height = 0;
            // Advances each time renderPrepass draws. RT64's present queue
            // calls render() once per frame it presents, interpolated frames
            // included, so while the pre-pass runs the gamma-dither noise
            // changes every presented frame; frames it skips (VI filtering
            // off, nothing shown) don't advance it.
            uint32_t frameCounter = 0;
        };
        Prepass prepass;]==])
        jfg_rt64_patch_replace([==[            bool removeBlackBorders = false;]==] [==[            bool removeBlackBorders = false;
            // The framebuffer the VI pass draws into, rebound after the
            // pre-pass. Without it the pre-pass doesn't run.
            const RenderFramebuffer *framebuffer = nullptr;
            // The frame's coverage estimate (front colour, coverage in alpha).
            // When set, the pre-pass reads it instead of `texture`.
            RenderTexture *coverageTexture = nullptr;]==])
        jfg_rt64_patch_replace([==[        void render(const RenderParams &p);]==] [==[        void render(const RenderParams &p);
        bool renderPrepass(const RenderParams &p);]==])
    elseif(relative STREQUAL "src/render/rt64_vi_renderer.cpp")
        jfg_rt64_patch_replace([==[#include "shared/rt64_video_interface.h"]==] [==[#include "shared/rt64_video_interface.h"

#include "n64_video/colour_reduction.hpp"
#include "n64_video/gamma_dither.hpp"
#include "n64_video/vi_filtering.hpp"]==])
        jfg_rt64_patch_replace([==[    void VIRenderer::render(const RenderParams &p) {]==] [==[    void VIRenderer::render(const RenderParams &frameParams) {
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
        jfg_rt64_patch_replace([==[        interop::VideoInterfaceCB pushConstants;
        pushConstants.videoResolution = computeHDSize(hlslpp::float2(p.vi->fbSize()), p.resolutionScale, p.downsamplingScale);]==] [==[        interop::VideoInterfaceCB pushConstants = {};
        pushConstants.videoResolution = videoResolution;]==])
        jfg_rt64_patch_replace([==[    void VIRenderer::getViewportAndScissor(]==] [==[    // Draws the frame into prepass.texture, one row per framebuffer row and
    // one column per sample of the active scanline: reduced to the console's
    // 16-bit colour (16-bit framebuffers only), anti-aliased or
    // dither-filtered, divot-filtered, resampled and given gamma-dither noise
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

        // With a coverage estimate the pre-pass reads it instead of the
        // averaged frame: each pixel's front colour and coverage.
        const RenderTexture *input = p.texture;
        if (p.coverageTexture != nullptr) {
            p.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(p.coverageTexture, RenderTextureLayout::SHADER_READ));
            input = p.coverageTexture;
        }

        prepass.descriptorSet->setTexture(prepass.descriptorSet->gInput, input, RenderTextureLayout::SHADER_READ);

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
        pushConstants.coverageInput = (p.coverageTexture != nullptr) ? 1 : 0;
        pushConstants.viAntiAliasing = filtering.anti_aliasing ? 1 : 0;
        pushConstants.divot = filtering.divot ? 1 : 0;
        n64_video::report_gamma_dither(filtering.gamma_dither, viControl);
        pushConstants.gammaDither = (filtering.gamma_dither == n64_video::GammaDither::noise) ? 1 : 0;
        pushConstants.noiseFrame = prepass.frameCounter++;

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
        jfg_rt64_patch_replace([==[                    renderParams.removeBlackBorders = removeBlackBorders;]==] [==[                    renderParams.removeBlackBorders = removeBlackBorders;
                    renderParams.framebuffer = swapChainFramebuffer;]==])
        jfg_rt64_patch_replace([==[                        renderParams.textureHeight = colorTarget->height;]==] [==[                        renderParams.textureHeight = colorTarget->height;
                        renderParams.coverageTexture = coverageEstimateTexture(colorTarget);]==])
    elseif(relative STREQUAL "src/shared/rt64_frame_params.h")
        jfg_rt64_patch_replace([==[        float ditherNoiseStrength;]==] [==[        float ditherNoiseStrength;
        // Video core coverage estimate: 1 makes the raster shader mark the
        // samples of anti-aliased draws in the colour target's alpha.
        uint coverageRecord;]==])
    elseif(relative STREQUAL "src/shaders/RasterPS.hlsl")
        jfg_rt64_patch_replace([==[#include "TextureSampler.hlsli"]==] [==[#include "TextureSampler.hlsli"
#include "CoverageRecord.hlsli"]==])
        jfg_rt64_patch_replace([==[    // Add highlight color to the last step.]==] [==[    // Video core coverage estimate: mark the sample when the draw
    // anti-aliases, next to RT64's coverage value.
    if (FrParams.coverageRecord != 0) {
        resultColor.a = CoverageRecordAlpha(otherMode, resultColor.a, cvgRange);
    }

    // Add highlight color to the last step.]==])
    elseif(relative STREQUAL "src/render/rt64_render_target.h")
        jfg_rt64_patch_replace([==[#include "rt64_render_worker.h"]==] [==[#include "rt64_render_worker.h"
#include "rt64_coverage_estimate.h"]==])
        jfg_rt64_patch_replace([==[        bool usesHDR = false;]==] [==[        bool usesHDR = false;
        CoverageEstimate coverageEstimate;]==])
    elseif(relative STREQUAL "src/shaders/TextureResolvePS.hlsl")
        # Each sample loses the record before the average, so the resolved
        # alpha is RT64's own.
        jfg_rt64_patch_replace([==[#include "shared/rt64_texture_copy.h"]==] [==[#include "shared/rt64_texture_copy.h"
#include "CoverageRecord.hlsli"]==])
        jfg_rt64_patch_replace([==[gInput.Load(pixelPos, ]==] [==[ResolveSampleLoad(pixelPos, ]==])
        jfg_rt64_patch_replace([==[Texture2DMS<float4> gInput : register(t1);]==] [==[Texture2DMS<float4> gInput : register(t1);

// Video core coverage estimate: one sample as RT64 computed it.
float4 ResolveSampleLoad(uint2 pixelPos, uint sampleIndex) {
    return CoverageRecordStripped(gInput.Load(pixelPos, sampleIndex), gConstants.coverageRecordAlphaRange);
}]==])
    elseif(relative STREQUAL "src/shared/rt64_texture_copy.h")
        jfg_rt64_patch_replace([==[        float2 uvScale;]==] [==[        float2 uvScale;
        // Video core coverage estimate: the colour target's alpha range (255,
        // or 65535 with HDR) when its samples carry the record, which a
        // resolve takes off; else 0.
        float coverageRecordAlphaRange;]==])
    elseif(relative STREQUAL "src/render/rt64_render_target.cpp")
        jfg_rt64_patch_replace([==[        fbWriteDescSet.reset();
    }]==] [==[        fbWriteDescSet.reset();
        coverageEstimate.release();
    }]==])
        jfg_rt64_patch_replace([==[        resolvedTextureDirty = true;]==] [==[        resolvedTextureDirty = true;
        coverageEstimate.valid = false;]==])
        # A hardware resolve can't take the record off each sample first.
        jfg_rt64_patch_replace([==[        const bool hwResolve = shaderLibrary->usesHardwareResolve && worker->device->getCapabilities().resolveRegion;]==] [==[        const bool hwResolve = shaderLibrary->usesHardwareResolve && worker->device->getCapabilities().resolveRegion && !coverageRecordNeedsRasterResolve();]==])
        jfg_rt64_patch_replace([==[        const bool hwResolve = shaderLibrary->usesHardwareResolve;]==] [==[        const bool hwResolve = shaderLibrary->usesHardwareResolve && !coverageRecordNeedsRasterResolve();]==])
        jfg_rt64_patch_replace([==[        textureCopyCB.uvScale.y = float(height);]==] [==[        textureCopyCB.uvScale.y = float(height);
        textureCopyCB.coverageRecordAlphaRange = coverageRecordAlphaRange(usesHDR);]==])
    elseif(relative STREQUAL "src/render/rt64_framebuffer_renderer.cpp")
        jfg_rt64_patch_replace([==[#include "rt64_framebuffer_renderer.h"]==] [==[#include "rt64_framebuffer_renderer.h"

#include "rt64_coverage_estimate.h"]==])
        jfg_rt64_patch_replace([==[        frameParams.ditherNoiseStrength = ditherNoiseStrength;]==] [==[        frameParams.ditherNoiseStrength = ditherNoiseStrength;
        frameParams.coverageRecord = writesCoverageRecord(multisampling) ? 1 : 0;]==])
    elseif(relative STREQUAL "src/render/rt64_raster_shader.cpp")
        jfg_rt64_patch_replace([==[#include "rt64_raster_shader.h"]==] [==[#include "rt64_raster_shader.h"

#include "n64_video/coverage_estimate.hpp"
#include "n64_video/video_options.hpp"]==])
        jfg_rt64_patch_replace([==[            auto &locations = multisampling.sampleLocations;
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
        jfg_rt64_patch_replace([==[            framebufferRenderer->recordSetup(]==] [==[            beginCoverageEstimateFrame();
            framebufferRenderer->recordSetup(]==])
        jfg_rt64_patch_replace([==[                    // Do the resolve if using MSAA while target override is active]==] [==[                    // Video core: estimate the pair's coverage for the VI pass.
                    estimatePairCoverage(ext.workloadGraphicsWorker, ext.shaderLibrary, targetManager.multisampling, colorTarget, depthTarget);

                    // Do the resolve if using MSAA while target override is active]==])
        jfg_rt64_patch_replace([==[                        overrideTarget->resolveFromTarget(ext.workloadGraphicsWorker, colorTarget, ext.shaderLibrary);]==] [==[                        overrideTarget->resolveFromTarget(ext.workloadGraphicsWorker, colorTarget, ext.shaderLibrary);
                        copyCoverageEstimate(ext.workloadGraphicsWorker, overrideTarget, colorTarget);]==])
    else()
        message(FATAL_ERROR "No video core RT64 patch for ${relative}")
    endif()
    set(contents "${contents}" PARENT_SCOPE)
endfunction()

# Patches the files RT64's own build reads (shaders, headers, its
# CMakeLists.txt) in a build-private copy of the RT64 source, before
# add_subdirectory, on top of the versions earlier patches wrote there (the
# fog patch), and adds the patch's new RT64 files. `original_root` is the
# pinned checkout, `source_view` the copy RT64 is built from.
function(jfg_n64_video_patch_source_view original_root source_view)
    # Paths relative to the video core folder.
    set(core_files "${PROJECT_SOURCE_DIR}/src/renderer/n64_video")
    set(new_files
        "coverage_layout.h|src/shared/rt64_coverage_layout.h"
        "rt64_patch/CoverageEstimateCS.hlsl|src/shaders/CoverageEstimateCS.hlsl"
        "rt64_patch/CoverageRecord.hlsli|src/shaders/CoverageRecord.hlsli"
        "rt64_patch/rt64_coverage_estimate_params.h|src/shared/rt64_coverage_estimate_params.h"
        "rt64_patch/rt64_coverage_estimate.h|src/render/rt64_coverage_estimate.h")
    foreach(new_file IN LISTS new_files)
        jfg_rt64_patch_split_pair("${new_file}" name relative)
        # The copy happens at configure time, so editing a source reconfigures.
        set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${core_files}/${name}")
        file(READ "${core_files}/${name}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        jfg_rt64_patch_write("${source_view}/${relative}" "${contents}")
    endforeach()

    set(inputs
        "src/shaders/VideoInterfacePS.hlsl|f4990ef5edfbdece9c89938a0c4951b3a285f99be9e6d92a415433c0e227f961"
        "src/shared/rt64_video_interface.h|5d97c854e5d4d0f1f40c5cf06815265baf9218982bcdf698d2b9b9fcf48fdb68"
        "CMakeLists.txt|4d6cb49be103302c019405794995aac6b526b759f257f4786a7a2a00d4de3fa2"
        "src/render/rt64_shader_library.h|582214e47482b6f97b57b451a0051ff10711372c29b53e2dc870c398d8c1e075"
        "src/render/rt64_shader_library.cpp|72421375b232d5449f2f90f153c8452420084f43809c3234bc9727eddfde6ffd"
        "src/render/rt64_vi_renderer.h|b03a6ea437eefc7a86f163ab72fa04b800ada5b9aa4fc7a15cf60a01378a3bc1"
        "src/shared/rt64_frame_params.h|dee91d3ff78177821d02d4a27cc43606a58f9a54d7896c5d17b7762a9b29ff3c"
        "src/shaders/RasterPS.hlsl|957b283411550457573421015580f84ca750c68a050981dbc1a4fe2634507820"
        "src/shaders/TextureResolvePS.hlsl|141e8f74af1a9228d1b45879b74826b83e6f1cd0988d6bf1a9095d6ad69a34d4"
        "src/shared/rt64_texture_copy.h|121a219ca6a514fdcb04d77b127e221c1f0407e27f5df719d6453b1c6cf5edc0"
        "src/render/rt64_render_target.h|5c234505140abe973c98c3d06c0558f6f66f6c67266ecac362c250137b3290d1")
    foreach(input IN LISTS inputs)
        jfg_rt64_patch_split_pair("${input}" relative expected)
        jfg_rt64_patch_check_input("video core" "${original_root}" "${relative}" "${expected}")
        file(READ "${source_view}/${relative}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        jfg_n64_video_patch_contents("${relative}")
        jfg_rt64_patch_write("${source_view}/${relative}" "${contents}")
    endforeach()
endfunction()

# Patches RT64 translation units in the rt64 target, on top of whatever
# version of each unit the target compiles now (other patches may already
# have replaced it), points the target at the patched copy and adds the
# coverage estimate's own unit. Call it after every other patch of these
# units. `core_include` is the folder that holds n64_video/.
function(jfg_n64_video_patch_rt64_target original_root core_include)
    jfg_rt64_patch_target_units("video core" "${original_root}" jfg_n64_video_patch_contents
        "${CMAKE_CURRENT_BINARY_DIR}/generated/rt64-n64-video"
        "src/render/rt64_vi_renderer.cpp|0159b441fd4872ef30a49d7603f4570d8858084004c20dd6646f51039241fa7b"
        "src/hle/rt64_present_queue.cpp|023dc3f4bc74bf6863e37ad1781fb32eaa543a7713bd8b9a3757b2262a1a1e52"
        "src/render/rt64_render_target.cpp|0dee0d19a4be22c4fa34b7ef793e3d4f75a07ab0d31252f9a23a2a0355edb41d"
        "src/render/rt64_framebuffer_renderer.cpp|73870c23b2340611b930b668d52aec9e687fd74b7da310962b7b1ef35dee015d"
        "src/render/rt64_raster_shader.cpp|5e5de4bbd8a22192e857c277bae5af033b8606a6464aa73863d2efe0fd0a0d4d"
        "src/hle/rt64_workload_queue.cpp|6e53030e8899c42347b51e67879fcb9679ad487ac067c192612c432d85bca4a4")
    target_include_directories(rt64 PRIVATE "${core_include}")
    target_sources(rt64 PRIVATE
        "${PROJECT_SOURCE_DIR}/src/renderer/n64_video/rt64_patch/rt64_coverage_estimate.cpp")
endfunction()
