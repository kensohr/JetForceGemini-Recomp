//
// Video core coverage estimate: the per-sample render-mode record in the
// colour target's alpha. The coverage patch copies this file to RT64's
// src/shaders/; RasterPS writes the record and TextureResolvePS takes it off
// before averaging.
//
// The layout is n64_video's coverage_layout.h; the rule for which draws
// record partial coverage follows n64_video::records_partial
// (coverage_estimate.hpp), here on RT64's other mode.
//

#pragma once

#include "shared/rt64_coverage_layout.h"
#include "shared/rt64_other_mode.h"

uint CoverageAlphaValue(float alpha, float alphaRange) {
    return uint(round(saturate(alpha) * alphaRange));
}

// The colour target's alpha for a sample: RT64's own coverage value
// (`rt64Alpha`), plus the partial record for an anti-aliased clamped draw
// (AA_EN with CVG_DST_CLAMP, not copy or fill). Every other draw writes
// RT64's value unchanged: CVG_DST_FULL, copy, fill and draws without AA_EN
// read as a full pixel, and CVG_DST_SAVE and CVG_DST_WRAP, which RT64 blends
// additively, keep the record of the draw under them.
float CoverageRecordAlpha(OtherMode otherMode, float rt64Alpha, float alphaRange) {
    const bool copyOrFill = (otherMode.cycleType() == G_CYC_COPY) || (otherMode.cycleType() == G_CYC_FILL);
    const bool partial = (otherMode.cvgDst() == CVG_DST_CLAMP) && otherMode.aaEn() && !copyOrFill;
    if (!partial) {
        return rt64Alpha;
    }

    return float(CoverageAlphaValue(rt64Alpha, alphaRange) + PartialRecordMarker) / alphaRange;
}

// A sample as RT64 computed it, the record taken off its alpha. `alphaRange`
// is 0 when no record is written.
float4 CoverageRecordStripped(float4 sample, float alphaRange) {
    if (alphaRange == 0.0f) {
        return sample;
    }

    const uint fullValue = uint(alphaRange);
    sample.a = float(Rt64CoverageValue(CoverageAlphaValue(sample.a, alphaRange), fullValue)) / alphaRange;
    return sample;
}
