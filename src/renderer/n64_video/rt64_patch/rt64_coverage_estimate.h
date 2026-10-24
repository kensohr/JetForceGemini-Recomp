//
// Video core coverage estimate: per colour target, the estimated RDP coverage
// and front-surface colour the VI pre-pass reads. The coverage patch copies
// this file to RT64's src/render/; rt64_coverage_estimate.cpp implements it.
//

#pragma once

#include <memory>
#include <vector>

#include "rt64_descriptor_sets.h"

namespace RT64 {
    struct RenderTarget;
    struct RenderWorker;
    struct ShaderLibrary;

    struct CoverageEstimateDescriptorSet : RenderDescriptorSetBase {
        uint32_t gColor;
        uint32_t gDepth;
        uint32_t gEstimate;
        uint32_t gFrontSamples;

        CoverageEstimateDescriptorSet(RenderDevice *device = nullptr) {
            builder.begin();
            gColor = builder.addTexture(1);
            gDepth = builder.addTexture(2);
            gEstimate = builder.addReadWriteTexture(3);
            gFrontSamples = builder.addReadWriteTexture(4);
            builder.end();

            if (device != nullptr) {
                create(device);
            }
        }
    };

    // Owned by a RenderTarget. Valid from the estimate after the pair that
    // last wrote the target until anything marks the target for resolve.
    struct CoverageEstimate {
        // rgb: front-surface colour; a: coverage as the RDP stores it /
        // EstimateCoverageScale (n64_video/coverage_layout.h).
        std::unique_ptr<RenderTexture> estimate;
        // The front surface's samples, one bit per sample.
        std::unique_ptr<RenderTexture> frontSamples;
        // One descriptor set per estimate recorded in the same frame, so a
        // set isn't rewritten while the frame's command list still uses it.
        std::vector<std::unique_ptr<CoverageEstimateDescriptorSet>> descriptorSets;
        uint32_t descriptorSetsUsed = 0;
        uint64_t descriptorSetsFrame = 0;
        // The frame whose depth last filled frontSamples.
        uint64_t frontSamplesFrame = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool valid = false;

        void release();
    };

    // Whether the raster shader marks anti-aliased samples in colour targets
    // drawn with `multisampling`: under the coverage option, and only with
    // MSAA, because only a resolve takes the mark off before RT64 reads the
    // target.
    bool writesCoverageRecord(const RenderMultisampling &multisampling);

    // Whether resolves must take RT64's raster path, which takes the mark off
    // each sample before averaging; a hardware resolve can't.
    bool coverageRecordNeedsRasterResolve();

    // The alpha range a resolve takes the mark off at, or 0 when no colour
    // target carries it.
    float coverageRecordAlphaRange(bool usesHDR);

    // Starts a frame's recording on the workload thread: descriptor sets of
    // the previous frame may be reused.
    void beginCoverageEstimateFrame();

    // Estimates the coverage of the framebuffer pair just recorded into
    // colorTarget, with its depth target (null when the pair has none).
    void estimatePairCoverage(RenderWorker *worker, const ShaderLibrary *shaderLibrary, const RenderMultisampling &multisampling,
        RenderTarget *colorTarget, RenderTarget *depthTarget);

    // Gives an interpolation target the estimate of the target it was resolved from.
    void copyCoverageEstimate(RenderWorker *worker, RenderTarget *dstTarget, RenderTarget *srcTarget);

    // The estimate for the VI pre-pass, or null when the target has no valid one.
    RenderTexture *coverageEstimateTexture(const RenderTarget *target);
};
