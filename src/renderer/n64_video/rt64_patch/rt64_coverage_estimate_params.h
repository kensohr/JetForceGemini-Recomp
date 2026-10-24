//
// Video core coverage estimate: push constants of CoverageEstimateCS.
// The coverage patch copies this file to RT64's src/shared/.
//

#pragma once

#include "shared/rt64_hlsl.h"

#ifdef HLSL_CPU
namespace interop {
#endif
    struct CoverageEstimateCB {
        // Size of the colour target, one rendered pixel per RDP pixel.
        uint2 resolution;
        uint sampleCount;
        // Bit 2s: sample s of an RDP pixel lies in the rendered pixel to the
        // right; bit 2s + 1: in the one below (n64_video::sample_pixel_offset_bits).
        uint samplePixelOffsets;
        // Samples that lie in the RDP pixel's own rendered pixel.
        uint ownPixelMask;
        // 1: find the front surface from depth and store it. 0: reuse the
        // stored front surface (a pair without a depth target).
        uint useDepth;
        // With useDepth 0: 1 when the stored front surface is from this frame,
        // else every sample counts as front.
        uint storedFrontValid;
        // The colour target's alpha range: 255, or 65535 with HDR.
        float alphaRange;
        float depthToleranceFactor;
        float minimumDepthTolerance;
    };
#ifdef HLSL_CPU
};
#endif
