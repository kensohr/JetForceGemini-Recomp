//
// Video core coverage estimate: the per-sample render-mode record the raster
// shader writes into the colour target's alpha. The coverage patch copies
// this file to RT64's src/shaders/ and calls it from RasterPS.
//
// It follows n64_video::sample_record and encode_sample_record
// (n64_video/coverage_estimate.hpp); keep the two in step.
//

#pragma once

#include "shared/rt64_other_mode.h"

// The colour target's alpha with the draw's record added above RT64's own
// coverage value, whose low 3 bits RT64 writes back to RDRAM.
// - CVG_DST_SAVE and CVG_DST_WRAP: 0. RT64 blends these destinations' alpha
//   additively, so 0 keeps the record already in the sample.
// - CVG_DST_FULL, copy and fill: a full pixel.
// - CVG_DST_CLAMP: partial with AA_EN, else a full pixel; CVG_X_ALPHA scales
//   the sample's eighths by alpha.
float CoverageRecordAlpha(OtherMode otherMode, float rt64Alpha, float alpha, float alphaRange) {
    const uint cvgDst = otherMode.cvgDst();
    if ((cvgDst == CVG_DST_SAVE) || (cvgDst == CVG_DST_WRAP)) {
        return 0.0f;
    }

    const uint SampleFullEighths = 8;
    const bool copyOrFill = (otherMode.cycleType() == G_CYC_COPY) || (otherMode.cycleType() == G_CYC_FILL);
    bool partial = false;
    uint eighths = SampleFullEighths;
    if ((cvgDst == CVG_DST_CLAMP) && !copyOrFill) {
        partial = otherMode.aaEn();
        if (otherMode.cvgXAlpha()) {
            eighths = uint(round(saturate(alpha) * float(SampleFullEighths)));
        }
    }

    const uint rt64Value = uint(round(saturate(rt64Alpha) * alphaRange)) & 0x7;
    const uint code = 1 + eighths + (partial ? (SampleFullEighths + 1) : 0);
    return float(rt64Value | (code << 3)) / alphaRange;
}
