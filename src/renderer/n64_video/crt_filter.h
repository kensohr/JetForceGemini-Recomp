//
// Video core CRT filter: the CRT preset table and the per-channel arithmetic
// of the CRT pass, shared by the CRT shader include (CrtFilter.hlsli) and its
// C++ tests. C++ includes this file directly; the video core's RT64 patch
// copies it next to CrtFilter.hlsli for the shaders. It is written in the
// subset C++ and HLSL have in common; __HLSL_VERSION (defined by DXC) tells
// them apart. Nothing here knows RT64 or the N64.
//
// The look follows Timothy Lottes' public-domain CRT shader: each output pixel
// mixes the three nearest scanlines with Gaussian beam weights and each
// scanline is a horizontal Gaussian of its source pixels, in linear light,
// then a phosphor mask. The code is our own (ADR 0004: no GPL shader code).
//

#pragma once

#ifdef __HLSL_VERSION
#define CRT_FILTER_CONSTANT static const
#define CRT_FILTER_FUNCTION
#else
#include <cmath>
namespace n64_video::crt {
using uint = unsigned;
using std::exp2;
using std::pow;
#define CRT_FILTER_CONSTANT inline constexpr
#define CRT_FILTER_FUNCTION inline
#endif

// CRT preset ids, the index into CrtPresets. Off doesn't run the CRT pass.
CRT_FILTER_CONSTANT uint CrtPresetOff = 0;
CRT_FILTER_CONSTANT uint CrtPresetRgbMonitor = 1;
CRT_FILTER_CONSTANT uint CrtPresetCount = 2;

// Phosphor mask types.
// Aperture grille: vertical RGB stripes, one triad per three output columns.
CRT_FILTER_CONSTANT uint CrtMaskApertureGrille = 0;
CRT_FILTER_CONSTANT uint CrtMaskTriadColumns = 3;

struct CrtPresetParams {
    // Scanline beam: weight exp2(scanlineHardness * d * d) at d scanlines from
    // the line's centre (Lottes' hardScan). More negative is a thinner beam.
    float scanlineHardness;
    // Horizontal beam: weight exp2(pixelHardness * d * d) at d source pixels
    // (Lottes' hardPix). More negative is sharper.
    float pixelHardness;
    // Mask: a lit channel is multiplied by maskLight, the others by maskDark.
    uint maskType;
    float maskDark;
    float maskLight;
    // Makes up the light scanlines and mask take away, before clipping at 1.
    float brightnessBoost;
    // Decodes the signal to linear light as the tube does.
    float crtGamma;
};

// Starting constants; the developer tunes them by eye. RGB monitor starts
// from Lottes' defaults (hardScan -8, hardPix -3, mask 0.5 / 1.5) with his
// shadow mask swapped for an aperture grille, and a boost that keeps full
// white at about 70 % of its unfiltered brightness.
CRT_FILTER_CONSTANT CrtPresetParams CrtPresets[CrtPresetCount] = {
    // Off: unused, the CRT pass doesn't run.
    { 0.0f, 0.0f, CrtMaskApertureGrille, 1.0f, 1.0f, 1.0f, 2.4f },
    // RGB monitor: crisp, pronounced scanlines, aperture grille.
    { -8.0f, -3.0f, CrtMaskApertureGrille, 0.5f, 1.5f, 1.6f, 2.4f },
};

// Above this many framebuffer rows the frame is interlaced and each scanline
// shows a pair of rows (no interlace flicker).
CRT_FILTER_CONSTANT uint CrtMaxProgressiveRows = 288;

// Each output pixel mixes the scanline it lies on and its two neighbours, so
// a scanline reaches 1.5 scanlines from its centre.
CRT_FILTER_CONSTANT float CrtScanlineReach = 1.5f;

CRT_FILTER_FUNCTION uint CrtRowsPerScanline(uint sourceRows) {
    return (sourceRows > CrtMaxProgressiveRows) ? 2 : 1;
}

CRT_FILTER_FUNCTION uint CrtScanlineCount(uint sourceRows) {
    return sourceRows / CrtRowsPerScanline(sourceRows);
}

// Where an output position (pixel centre, in pixels from the picture's top)
// lies, in scanlines from the picture's top.
CRT_FILTER_FUNCTION float CrtScanlinePosition(float outputY, float outputHeight, uint scanlineCount) {
    return outputY / outputHeight * float(scanlineCount);
}

CRT_FILTER_FUNCTION float CrtGaussian(float distance, float hardness) {
    return exp2(hardness * distance * distance);
}

// The beam weight of a scanline `distance` scanlines away.
CRT_FILTER_FUNCTION float CrtScanlineWeight(float distance, uint preset) {
    return CrtGaussian(distance, CrtPresets[preset].scanlineHardness);
}

// The weight of a source pixel `distance` source pixels away, before the
// row's taps are normalised.
CRT_FILTER_FUNCTION float CrtPixelWeight(float distance, uint preset) {
    return CrtGaussian(distance, CrtPresets[preset].pixelHardness);
}

// The mask's factor for one channel (0 red, 1 green, 2 blue) of an output
// pixel column. Aligned to whole output pixels, so it can't beat with the
// pixel grid.
CRT_FILTER_FUNCTION float CrtMask(uint column, uint channel, uint preset) {
    const bool lit = (column % CrtMaskTriadColumns) == channel;
    return lit ? CrtPresets[preset].maskLight : CrtPresets[preset].maskDark;
}

CRT_FILTER_FUNCTION float CrtDecode(float signal, uint preset) {
    return pow(signal, CrtPresets[preset].crtGamma);
}

CRT_FILTER_FUNCTION float CrtClamp01(float value) {
    return (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
}

// One channel of an output pixel in linear light: the scanline it lies on
// (`centre`) and the ones above and below, each already filtered along its
// row, mixed by beam weight, boosted, masked and clipped. `distance` is the
// pixel's offset from the centre scanline, in scanlines (-0.5 to 0.5).
CRT_FILTER_FUNCTION float CrtChannel(float above, float centre, float below, float distance, uint column, uint channel, uint preset) {
    const float beam =
        above * CrtScanlineWeight(distance + 1.0f, preset) +
        centre * CrtScanlineWeight(distance, preset) +
        below * CrtScanlineWeight(distance - 1.0f, preset);
    return CrtClamp01(beam * CrtPresets[preset].brightnessBoost * CrtMask(column, channel, preset));
}

// Linear light to the sRGB signal a UNORM swapchain shows.
CRT_FILTER_FUNCTION float CrtEncodeSrgb(float light) {
    return (light <= 0.0031308f) ? (light * 12.92f) : (1.055f * pow(light, 1.0f / 2.4f) - 0.055f);
}

#ifndef __HLSL_VERSION
} // namespace n64_video::crt
#endif

#undef CRT_FILTER_CONSTANT
#undef CRT_FILTER_FUNCTION
