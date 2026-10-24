//
// Video core coverage estimate: estimates each RDP pixel's coverage and
// front-surface colour from a colour target's MSAA samples, for the VI
// pre-pass's anti-aliasing and divot filter. The coverage patch copies this
// file to RT64's src/shaders/.
//
// It follows the reference in n64_video/coverage_estimate.hpp
// (front_depth_tolerance, front_surface_mask, pixel_coverage); keep the two
// in step. The record's layout and decoding are shared with it through
// rt64_coverage_layout.h (n64_video/coverage_layout.h).
//

#include "shared/rt64_coverage_estimate_params.h"
#include "shared/rt64_coverage_layout.h"

#define BLOCK_SIZE 8

[[vk::push_constant]] ConstantBuffer<CoverageEstimateCB> gConstants : register(b0);
Texture2DMS<float4> gColor : register(t1);
Texture2DMS<float> gDepth : register(t2);
// rgb: front-surface colour; a: coverage as the RDP stores it (0-7) /
// EstimateCoverageScale.
RWTexture2D<float4> gEstimate : register(u3);
// The front surface's samples, one bit per sample. Read back by pairs
// without a depth target; the format lets Vulkan read it without
// shaderStorageImageReadWithoutFormat.
[[vk::image_format("r32ui")]] RWTexture2D<uint> gFrontSamples : register(u4);

static const uint MaxSamples = 8;

struct SampleRecord {
    bool partial;
    uint eighths;
};

// What the draws left in the sample's alpha (CoverageRecord.hlsli), as
// n64_video::decode_sample_record reads it.
SampleRecord DecodeSampleRecord(float alpha) {
    const uint value = uint(round(saturate(alpha) * gConstants.alphaRange));
    const uint fullValue = uint(gConstants.alphaRange);
    SampleRecord record;
    record.partial = IsPartialRecord(value, fullValue);
    record.eighths = SampleEighths(Rt64CoverageValue(value, fullValue));
    return record;
}

int2 ClampToTarget(int2 pixel) {
    return clamp(pixel, int2(0, 0), int2(gConstants.resolution) - 1);
}

// The rendered pixel that holds sample s of RDP pixel `pixel`.
int2 SamplePixel(int2 pixel, uint s) {
    const uint bits = gConstants.samplePixelOffsets >> (2 * s);
    return ClampToTarget(pixel + int2(bits & 1, (bits >> 1) & 1));
}

// The nearest sample depth of one rendered pixel.
float RenderedFrontDepth(int2 pixel) {
    pixel = ClampToTarget(pixel);
    float front = gDepth.Load(pixel, 0);
    for (uint s = 1; s < gConstants.sampleCount; s++) {
        front = min(front, gDepth.Load(pixel, s));
    }

    return front;
}

// 8 * dz, dz the local depth slope per axis taken as the smaller difference
// to the two neighbours, so a silhouette on one side doesn't widen it.
float FrontDepthTolerance(int2 pixel) {
    const float centre = RenderedFrontDepth(pixel);
    const float slopeX = min(abs(centre - RenderedFrontDepth(pixel + int2(-1, 0))), abs(RenderedFrontDepth(pixel + int2(1, 0)) - centre));
    const float slopeY = min(abs(centre - RenderedFrontDepth(pixel + int2(0, -1))), abs(RenderedFrontDepth(pixel + int2(0, 1)) - centre));
    return gConstants.depthToleranceFactor * (slopeX + slopeY) + gConstants.minimumDepthTolerance;
}

uint FrontSurfaceFromDepth(int2 pixel) {
    float depths[MaxSamples];
    float front = 1.0f;
    for (uint s = 0; s < gConstants.sampleCount; s++) {
        depths[s] = gDepth.Load(SamplePixel(pixel, s), s);
        front = (s == 0) ? depths[s] : min(front, depths[s]);
    }

    const float limit = front + FrontDepthTolerance(pixel);
    uint mask = 0;
    for (uint s = 0; s < gConstants.sampleCount; s++) {
        if (depths[s] <= limit) {
            mask |= 1U << s;
        }
    }

    return mask;
}

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void CSMain(uint2 coord : SV_DispatchThreadID) {
    if ((coord.x >= gConstants.resolution.x) || (coord.y >= gConstants.resolution.y)) {
        return;
    }

    const int2 pixel = int2(coord);
    const uint count = gConstants.sampleCount;
    const uint allSamples = (1U << count) - 1;
    uint front = allSamples;
    if (gConstants.useDepth != 0) {
        front = FrontSurfaceFromDepth(pixel);
        gFrontSamples[coord] = front;
    }
    else if (gConstants.storedFrontValid != 0) {
        front = gFrontSamples[coord] & allSamples;
    }

    if (front == 0) {
        front = allSamples;
    }

    // An anti-aliased front covers the share of samples it covers; anything
    // else is drawn whole by its top-left sample (sample 0).
    bool frontPartial = true;
    uint frontEighths = 0;
    for (uint s = 0; s < count; s++) {
        if ((front & (1U << s)) != 0) {
            const SampleRecord record = DecodeSampleRecord(gColor.Load(SamplePixel(pixel, s), s).a);
            frontPartial = frontPartial && record.partial;
            frontEighths += record.eighths;
        }
    }

    uint coveredEighths;
    uint colourMask;
    if (frontPartial) {
        coveredEighths = (frontEighths + count / 2) / count;
        const uint ownFront = front & gConstants.ownPixelMask;
        colourMask = (ownFront != 0) ? ownFront : front;
    }
    else {
        const SampleRecord topLeft = DecodeSampleRecord(gColor.Load(pixel, 0).a);
        coveredEighths = topLeft.partial ? SampleFullEighths : topLeft.eighths;
        colourMask = 1;
    }

    const uint coverage = max(min(coveredEighths, SampleFullEighths), 1U) - 1;
    float3 colour = 0.0f;
    uint colourSamples = 0;
    for (uint s = 0; s < count; s++) {
        if ((colourMask & (1U << s)) != 0) {
            colour += gColor.Load(SamplePixel(pixel, s), s).rgb;
            colourSamples++;
        }
    }

    gEstimate[coord] = float4(colour / float(colourSamples), float(coverage) / EstimateCoverageScale);
}
