//
// Video core coverage estimate: runs CoverageEstimateCS after each
// framebuffer pair on the workload thread and keeps the result with the
// colour target. Compiled into RT64 by the video core's patch
// (cmake/rt64_n64_video.cmake). The decisions it applies live in
// n64_video/coverage_estimate.hpp.
//

// The declarations come through rt64_render_target.h, from the copy of
// rt64_coverage_estimate.h the patch puts in RT64's src/render/. Including
// this folder's copy too would define its types twice.
#include "rt64_render_target.h"

#include <array>
#include <atomic>

#include "n64_video/coverage_estimate.hpp"
#include "n64_video/video_options.hpp"
#include "shared/rt64_coverage_estimate_params.h"

#include "rt64_render_worker.h"
#include "rt64_shader_library.h"

namespace RT64 {
    namespace {
        // The estimate shader's work group size, as CoverageEstimateCS.hlsl.
        constexpr uint32_t CoverageBlockSize = 8;
        constexpr float AlphaRange8Bit = 255.0f;
        constexpr float AlphaRange16Bit = 65535.0f;

        // Counts recordings on the workload thread, the only thread that
        // records estimates.
        uint64_t coverageFrame = 0;

        void reportOnce(n64_video::CoverageStatus status, const n64_video::CoverageTarget &target) {
            static std::array<std::atomic<bool>, size_t(n64_video::CoverageStatus::count)> reported = {};
            if (reported[size_t(status)].exchange(true)) {
                return;
            }

            const n64_video::CoverageLogSink sink = n64_video::coverage_log_sink();
            if (sink != nullptr) {
                sink(n64_video::coverage_status_log_level(status), n64_video::coverage_status_message(status, target).c_str());
            }
        }

        // The RDP's sample positions are in use when the video core asked for
        // the coverage estimate and RT64 renders MSAA with sample locations
        // (see the coverage patch of RasterShader::generateMultisamplingPattern).
        n64_video::CoverageTarget coverageTarget(const RenderMultisampling &multisampling, const RenderTarget &target) {
            n64_video::CoverageTarget result;
            result.sample_count = uint32_t(multisampling.sampleCount);
            result.rdp_sample_positions = multisampling.sampleLocationsEnabled;
            result.resolution_scale_x = target.resolutionScale.x;
            result.resolution_scale_y = target.resolutionScale.y;
            result.downsample_multiplier = target.downsampleMultiplier;
            return result;
        }

        void setupTextures(RenderWorker *worker, CoverageEstimate &coverage, uint32_t width, uint32_t height) {
            if ((coverage.estimate != nullptr) && (coverage.width == width) && (coverage.height == height)) {
                return;
            }

            // Replacing textures while the frame records is safe here for the
            // same reason RT64 resizes render targets on this thread: the
            // workload waits for each recording's command list to finish.
            const RenderTextureFlags flags = RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS;
            coverage.estimate = worker->device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, RenderFormat::R8G8B8A8_UNORM, flags));
            coverage.estimate->setName("Render Target Coverage Estimate");
            coverage.frontSamples = worker->device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, RenderFormat::R32_UINT, flags));
            coverage.frontSamples->setName("Render Target Coverage Front Samples");
            coverage.width = width;
            coverage.height = height;
            coverage.frontSamplesFrame = 0;
        }

        CoverageEstimateDescriptorSet *nextDescriptorSet(RenderWorker *worker, CoverageEstimate &coverage) {
            if (coverage.descriptorSetsFrame != coverageFrame) {
                coverage.descriptorSetsFrame = coverageFrame;
                coverage.descriptorSetsUsed = 0;
            }

            if (coverage.descriptorSetsUsed == coverage.descriptorSets.size()) {
                coverage.descriptorSets.emplace_back(std::make_unique<CoverageEstimateDescriptorSet>(worker->device));
            }

            return coverage.descriptorSets[coverage.descriptorSetsUsed++].get();
        }
    }

    void CoverageEstimate::release() {
        estimate.reset();
        frontSamples.reset();
        descriptorSets.clear();
        descriptorSetsUsed = 0;
        width = 0;
        height = 0;
        valid = false;
    }

    bool writesCoverageRecord(const RenderMultisampling &multisampling) {
        return n64_video::estimates_coverage(n64_video::active_options()) && (multisampling.sampleCount > 1);
    }

    bool coverageRecordNeedsRasterResolve() {
        return n64_video::estimates_coverage(n64_video::active_options());
    }

    float coverageRecordAlphaRange(bool usesHDR) {
        if (!n64_video::estimates_coverage(n64_video::active_options())) {
            return 0.0f;
        }

        return usesHDR ? AlphaRange16Bit : AlphaRange8Bit;
    }

    void beginCoverageEstimateFrame() {
        coverageFrame++;
    }

    void estimatePairCoverage(RenderWorker *worker, const ShaderLibrary *shaderLibrary, const RenderMultisampling &multisampling,
        RenderTarget *colorTarget, RenderTarget *depthTarget)
    {
        if ((colorTarget == nullptr) || colorTarget->isEmpty() || !n64_video::estimates_coverage(n64_video::active_options())) {
            return;
        }

        CoverageEstimate &coverage = colorTarget->coverageEstimate;
        coverage.valid = false;
        const n64_video::CoverageTarget target = coverageTarget(multisampling, *colorTarget);
        const n64_video::CoverageStatus status = n64_video::coverage_status(target);
        if (status != n64_video::CoverageStatus::estimating) {
            reportOnce(status, target);
            return;
        }

        const bool useDepth = (depthTarget != nullptr) && !depthTarget->isEmpty() &&
            (depthTarget->width >= colorTarget->width) && (depthTarget->height >= colorTarget->height);
        const bool storedFrontValid = !useDepth && (coverage.frontSamplesFrame == coverageFrame);
        if (!useDepth && !storedFrontValid) {
            reportOnce(n64_video::CoverageStatus::no_depth, target);
        }

        setupTextures(worker, coverage, colorTarget->width, colorTarget->height);
        const n64_video::MsaaSamplePattern pattern = n64_video::msaa_sample_pattern(uint32_t(multisampling.sampleCount));
        interop::CoverageEstimateCB constants = {};
        constants.resolution = { colorTarget->width, colorTarget->height };
        constants.sampleCount = pattern.count;
        constants.samplePixelOffsets = n64_video::sample_pixel_offset_bits(pattern);
        constants.ownPixelMask = n64_video::own_pixel_mask(pattern);
        constants.useDepth = useDepth ? 1 : 0;
        constants.storedFrontValid = storedFrontValid ? 1 : 0;
        constants.alphaRange = colorTarget->usesHDR ? AlphaRange16Bit : AlphaRange8Bit;
        constants.depthToleranceFactor = n64_video::rdp_depth_tolerance_factor;
        constants.minimumDepthTolerance = n64_video::minimum_depth_tolerance;

        // Without a depth target the shader never reads gDepth; the colour
        // target fills the slot so the set stays complete.
        CoverageEstimateDescriptorSet *descriptorSet = nextDescriptorSet(worker, coverage);
        descriptorSet->setTexture(descriptorSet->gColor, colorTarget->texture.get(), RenderTextureLayout::SHADER_READ, colorTarget->textureView.get());
        if (useDepth) {
            descriptorSet->setTexture(descriptorSet->gDepth, depthTarget->texture.get(), RenderTextureLayout::DEPTH_READ, depthTarget->textureView.get());
        }
        else {
            descriptorSet->setTexture(descriptorSet->gDepth, colorTarget->texture.get(), RenderTextureLayout::SHADER_READ, colorTarget->textureView.get());
        }

        descriptorSet->setTexture(descriptorSet->gEstimate, coverage.estimate.get(), RenderTextureLayout::GENERAL);
        descriptorSet->setTexture(descriptorSet->gFrontSamples, coverage.frontSamples.get(), RenderTextureLayout::GENERAL);

        std::vector<RenderTextureBarrier> barriers = {
            RenderTextureBarrier(colorTarget->texture.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(coverage.estimate.get(), RenderTextureLayout::GENERAL),
            RenderTextureBarrier(coverage.frontSamples.get(), RenderTextureLayout::GENERAL)
        };

        if (useDepth) {
            barriers.emplace_back(depthTarget->texture.get(), RenderTextureLayout::DEPTH_READ);
        }

        RenderCommandList *commandList = worker->commandList.get();
        commandList->barriers(RenderBarrierStage::COMPUTE, barriers.data(), uint32_t(barriers.size()));
        const ShaderRecord &shader = shaderLibrary->coverageEstimate;
        commandList->setPipeline(shader.pipeline.get());
        commandList->setComputePipelineLayout(shader.pipelineLayout.get());
        commandList->setComputePushConstants(0, &constants);
        commandList->setComputeDescriptorSet(descriptorSet->get(), 0);
        commandList->dispatch((colorTarget->width + CoverageBlockSize - 1) / CoverageBlockSize, (colorTarget->height + CoverageBlockSize - 1) / CoverageBlockSize, 1);

        RenderTextureBarrier readBarriers[] = {
            RenderTextureBarrier(coverage.estimate.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(coverage.frontSamples.get(), RenderTextureLayout::GENERAL)
        };

        commandList->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COMPUTE, readBarriers, uint32_t(std::size(readBarriers)));
        if (useDepth) {
            coverage.frontSamplesFrame = coverageFrame;
        }

        coverage.valid = true;
        reportOnce(n64_video::CoverageStatus::estimating, target);
    }

    void copyCoverageEstimate(RenderWorker *worker, RenderTarget *dstTarget, RenderTarget *srcTarget) {
        if ((dstTarget == nullptr) || (srcTarget == nullptr)) {
            return;
        }

        CoverageEstimate &dst = dstTarget->coverageEstimate;
        const CoverageEstimate &src = srcTarget->coverageEstimate;
        dst.valid = false;
        if (!src.valid || (src.estimate == nullptr)) {
            return;
        }

        setupTextures(worker, dst, src.width, src.height);
        RenderCommandList *commandList = worker->commandList.get();
        RenderTextureBarrier copyBarriers[] = {
            RenderTextureBarrier(src.estimate.get(), RenderTextureLayout::COPY_SOURCE),
            RenderTextureBarrier(dst.estimate.get(), RenderTextureLayout::COPY_DEST)
        };

        commandList->barriers(RenderBarrierStage::COPY, copyBarriers, uint32_t(std::size(copyBarriers)));
        commandList->copyTexture(dst.estimate.get(), src.estimate.get());
        RenderTextureBarrier readBarriers[] = {
            RenderTextureBarrier(src.estimate.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(dst.estimate.get(), RenderTextureLayout::SHADER_READ)
        };

        commandList->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COMPUTE, readBarriers, uint32_t(std::size(readBarriers)));
        dst.valid = true;
    }

    RenderTexture *coverageEstimateTexture(const RenderTarget *target) {
        if ((target == nullptr) || !target->coverageEstimate.valid) {
            return nullptr;
        }

        return target->coverageEstimate.estimate.get();
    }
};
