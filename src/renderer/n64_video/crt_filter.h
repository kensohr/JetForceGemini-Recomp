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
using std::abs;
using std::exp2;
using std::pow;
using std::sqrt;
#define CRT_FILTER_CONSTANT inline constexpr
#define CRT_FILTER_FUNCTION inline
#endif

// CRT preset ids, the index into CrtPresets. Off doesn't run the CRT pass.
CRT_FILTER_CONSTANT uint CrtPresetOff = 0;
CRT_FILTER_CONSTANT uint CrtPresetRgbMonitor = 1;
CRT_FILTER_CONSTANT uint CrtPresetCompositeTv = 2;
CRT_FILTER_CONSTANT uint CrtPresetSvideoTv = 3;
CRT_FILTER_CONSTANT uint CrtPresetCount = 4;

// Phosphor mask types.
// Aperture grille: vertical RGB stripes, one triad per three output columns.
CRT_FILTER_CONSTANT uint CrtMaskApertureGrille = 0;
CRT_FILTER_CONSTANT uint CrtMaskTriadColumns = 3;
// Slot mask: the same triads, cut into slots by one dark row in every
// CrtSlotRows, with alternate triads' slots offset by half that.
CRT_FILTER_CONSTANT uint CrtMaskSlot = 1;
CRT_FILTER_CONSTANT uint CrtSlotRows = 4;

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
    // The video connection's blur of the signal, before the decode: luma and
    // chroma are each filtered along the row with a Gaussian this wide (full
    // width at half maximum, in source pixels; 0 is no blur), and chroma is
    // moved this many source pixels to the right.
    float lumaWidth;
    float chromaWidth;
    float chromaShift;
    // Screen curvature (Lottes' warp): how far the picture bends along x and
    // along y towards the edges. 0 is a flat axis.
    float curvatureX;
    float curvatureY;
};

// Starting constants; the developer tunes them by eye. RGB monitor starts
// from Lottes' defaults (hardScan -8, hardPix -3, mask 0.5 / 1.5) with his
// shadow mask swapped for an aperture grille, and a boost that keeps full
// white at about 70 % of its unfiltered brightness. The TV presets share
// faint scanlines (about 15 % darker between lines than on them) and a slot
// mask weaker than the grille, with a boost that keeps full white at about
// 73 %; they differ in the signal blur. Curvature starts from Lottes' warp;
// RGB monitor is a cylindrical tube, curved horizontally only.
CRT_FILTER_CONSTANT CrtPresetParams CrtPresets[CrtPresetCount] = {
    // Off: unused, the CRT pass doesn't run.
    { 0.0f, 0.0f, CrtMaskApertureGrille, 1.0f, 1.0f, 1.0f, 2.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
    // RGB monitor: crisp, pronounced scanlines, aperture grille. An RGB
    // connection doesn't blur the signal.
    { -8.0f, -3.0f, CrtMaskApertureGrille, 0.5f, 1.5f, 1.6f, 2.4f, 0.0f, 0.0f, 0.0f, 1.0f / 48.0f, 0.0f },
    // Composite TV: soft luma, wide chroma moved a little to the right, which
    // gives the colour bleed; faint scanlines.
    { -4.5f, -3.0f, CrtMaskSlot, 0.75f, 1.25f, 0.85f, 2.4f, 1.6f, 4.0f, 0.3f, 1.0f / 32.0f, 1.0f / 24.0f },
    // S-Video TV: luma and chroma on separate wires, so only slight luma blur
    // and moderate chroma blur, without a shift; faint scanlines.
    { -4.5f, -3.0f, CrtMaskSlot, 0.75f, 1.25f, 0.85f, 2.4f, 0.8f, 2.5f, 0.0f, 1.0f / 32.0f, 1.0f / 24.0f },
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

// Whether an output pixel lies in a slot mask's dark row.
CRT_FILTER_FUNCTION bool CrtSlotGap(uint column, uint row) {
    const uint triad = column / CrtMaskTriadColumns;
    const uint slotOffset = (triad % 2) * (CrtSlotRows / 2);
    return ((row + slotOffset) % CrtSlotRows) == (CrtSlotRows - 1);
}

// The mask's factor for one channel (0 red, 1 green, 2 blue) of an output
// pixel. Aligned to whole output pixels, so it can't beat with the pixel grid.
CRT_FILTER_FUNCTION float CrtMaskAt(uint column, uint row, uint channel, uint preset) {
    const bool inGap = (CrtPresets[preset].maskType == CrtMaskSlot) && CrtSlotGap(column, row);
    const bool lit = !inGap && ((column % CrtMaskTriadColumns) == channel);
    return lit ? CrtPresets[preset].maskLight : CrtPresets[preset].maskDark;
}

// The mask's factor in an output pixel column's first row; the aperture
// grille is the same in every row.
CRT_FILTER_FUNCTION float CrtMask(uint column, uint channel, uint preset) {
    return CrtMaskAt(column, 0, channel, preset);
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
// pixel's offset from the centre scanline, in scanlines (-0.5 to 0.5);
// `column` and `row` are the output pixel's, for the mask.
CRT_FILTER_FUNCTION float CrtChannelAt(float above, float centre, float below, float distance, uint column, uint row, uint channel, uint preset) {
    const float beam =
        above * CrtScanlineWeight(distance + 1.0f, preset) +
        centre * CrtScanlineWeight(distance, preset) +
        below * CrtScanlineWeight(distance - 1.0f, preset);
    return CrtClamp01(beam * CrtPresets[preset].brightnessBoost * CrtMaskAt(column, row, channel, preset));
}

// CrtChannelAt in an output pixel column's first row; enough for the
// aperture grille, which is the same in every row.
CRT_FILTER_FUNCTION float CrtChannel(float above, float centre, float below, float distance, uint column, uint channel, uint preset) {
    return CrtChannelAt(above, centre, below, distance, column, 0, channel, preset);
}

// The signal blur reads this many source pixels on each side of the one it
// filters; sized for the widest kernel, Composite TV's chroma.
CRT_FILTER_CONSTANT int CrtSignalKernelRadius = 5;
CRT_FILTER_CONSTANT uint CrtSignalKernelTaps = 2 * CrtSignalKernelRadius + 1;

// The signal blur's weights for the source pixels CrtSignalKernelRadius to
// the left (index 0) through as many to the right of the filtered one, each
// kernel summing to 1.
struct CrtSignalKernels {
    float luma[CrtSignalKernelTaps];
    float chroma[CrtSignalKernelTaps];
};

// A Gaussian `width` wide at half maximum; width 0 passes only distance 0.
CRT_FILTER_FUNCTION float CrtSignalWeight(float distance, float width) {
    if (width <= 0.0f) {
        return (distance == 0.0f) ? 1.0f : 0.0f;
    }
    // exp2(-4 d^2 / w^2) is 1/2 at d = w / 2.
    return exp2(-4.0f * distance * distance / (width * width));
}

CRT_FILTER_FUNCTION CrtSignalKernels CrtMakeSignalKernels(uint preset) {
    CrtSignalKernels kernels;
    float lumaTotal = 0.0f;
    float chromaTotal = 0.0f;
    for (uint tap = 0; tap < CrtSignalKernelTaps; tap++) {
        const float offset = float(int(tap) - CrtSignalKernelRadius);
        kernels.luma[tap] = CrtSignalWeight(offset, CrtPresets[preset].lumaWidth);
        // Chroma moved right by the shift is read from that far to the left.
        kernels.chroma[tap] = CrtSignalWeight(offset + CrtPresets[preset].chromaShift, CrtPresets[preset].chromaWidth);
        lumaTotal += kernels.luma[tap];
        chromaTotal += kernels.chroma[tap];
    }
    for (uint tap = 0; tap < CrtSignalKernelTaps; tap++) {
        kernels.luma[tap] /= lumaTotal;
        kernels.chroma[tap] /= chromaTotal;
    }
    return kernels;
}

// A colour as three numbers: RGB, or YIQ (x luma, y and z chroma).
struct CrtTriple {
    float x;
    float y;
    float z;
};

CRT_FILTER_FUNCTION CrtTriple CrtMakeTriple(float x, float y, float z) {
    CrtTriple triple = { x, y, z };
    return triple;
}

// NTSC's YIQ (FCC), in which a TV carries luma and chroma separately. The
// inverse is exact to the precision shown, so a round trip is the identity.
CRT_FILTER_FUNCTION CrtTriple CrtRgbToYiq(CrtTriple rgb) {
    return CrtMakeTriple(
        0.299f * rgb.x + 0.587f * rgb.y + 0.114f * rgb.z,
        0.595716f * rgb.x - 0.274453f * rgb.y - 0.321263f * rgb.z,
        0.211456f * rgb.x - 0.522591f * rgb.y + 0.311135f * rgb.z);
}

CRT_FILTER_FUNCTION CrtTriple CrtYiqToRgb(CrtTriple yiq) {
    return CrtMakeTriple(
        yiq.x + 0.956296f * yiq.y + 0.621024f * yiq.z,
        yiq.x - 0.272122f * yiq.y - 0.647381f * yiq.z,
        yiq.x - 1.106989f * yiq.y + 1.704615f * yiq.z);
}

// Source pixels on each side of the nearest one that a scanline's horizontal
// beam reads.
CRT_FILTER_CONSTANT int CrtRowTapRadius = 2;

// The source pixels of a row the CRT pass reads per scanline: the beam's taps
// and the signal blur's reach around each of them.
CRT_FILTER_CONSTANT uint CrtSignalWindowTaps = 2 * (CrtRowTapRadius + CrtSignalKernelRadius) + 1;

// The signal of window[centre] after the video connection's blur: luma and
// chroma filtered apart in YIQ, then back to RGB and clipped to the signal
// range. `centre` must be at least CrtSignalKernelRadius from both ends.
CRT_FILTER_FUNCTION CrtTriple CrtBlurSignal(CrtTriple window[CrtSignalWindowTaps], int centre, CrtSignalKernels kernels) {
    CrtTriple yiq = CrtMakeTriple(0.0f, 0.0f, 0.0f);
    for (uint tap = 0; tap < CrtSignalKernelTaps; tap++) {
        const CrtTriple sample = CrtRgbToYiq(window[centre - CrtSignalKernelRadius + int(tap)]);
        yiq.x += kernels.luma[tap] * sample.x;
        yiq.y += kernels.chroma[tap] * sample.y;
        yiq.z += kernels.chroma[tap] * sample.z;
    }
    const CrtTriple rgb = CrtYiqToRgb(yiq);
    return CrtMakeTriple(CrtClamp01(rgb.x), CrtClamp01(rgb.y), CrtClamp01(rgb.z));
}

// Linear light to the sRGB signal a UNORM swapchain shows.
CRT_FILTER_FUNCTION float CrtEncodeSrgb(float light) {
    return (light <= 0.0031308f) ? (light * 12.92f) : (1.055f * pow(light, 1.0f / 2.4f) - 0.055f);
}

// Screen curvature: the picture bends like a CRT tube's glass, darkens
// towards its corners (vignette) and has rounded corners. The warp maps an
// output position to the picture position the CRT pass samples there; the
// mask is not warped. Positions are in output pixels from the picture
// rectangle's top-left corner.

// Vignette: light left at the picture's corners is 1 - CrtVignetteStrength.
CRT_FILTER_CONSTANT float CrtVignetteStrength = 0.3f;
// Corner radius, as a fraction of the picture rectangle's shorter side.
CRT_FILTER_CONSTANT float CrtCornerRadius = 0.04f;

CRT_FILTER_FUNCTION float CrtMin(float a, float b) {
    return (a < b) ? a : b;
}

CRT_FILTER_FUNCTION float CrtMax(float a, float b) {
    return (a > b) ? a : b;
}

// A position from -1 (left or top edge) to 1 (right or bottom edge), and back.
CRT_FILTER_FUNCTION float CrtCentred(float position, float size) {
    return position / size * 2.0f - 1.0f;
}

CRT_FILTER_FUNCTION float CrtUncentred(float centred, float size) {
    return (centred + 1.0f) * 0.5f * size;
}

// Lottes' warp along one axis, in centred positions: the further the
// position lies along the other axis, the further it is pushed out.
CRT_FILTER_FUNCTION float CrtWarpAxis(float along, float across, float curvature) {
    return along * (1.0f + across * across * curvature);
}

// Picture fit: the scale on a warped axis that puts the curved picture inside
// the rectangle, edge midpoints on its edge and corners pulled inward, with
// nothing cropped. Other picture fits would scale here too.
CRT_FILTER_FUNCTION float CrtCurvatureFitScale(float curvature) {
    return 1.0f / CrtWarpAxis(1.0f, 0.0f, curvature);
}

CRT_FILTER_FUNCTION float CrtCurvedAxis(float along, float across, float alongSize, float acrossSize, float curvature) {
    const float centred = CrtWarpAxis(CrtCentred(along, alongSize), CrtCentred(across, acrossSize), curvature);
    return CrtUncentred(centred * CrtCurvatureFitScale(curvature), alongSize);
}

// The picture position the CRT pass samples at output position (x, y); the
// output position itself when curvature is off.
CRT_FILTER_FUNCTION float CrtCurvedX(float x, float y, float width, float height, uint preset, bool curvature) {
    return curvature ? CrtCurvedAxis(x, y, width, height, CrtPresets[preset].curvatureX) : x;
}

CRT_FILTER_FUNCTION float CrtCurvedY(float x, float y, float width, float height, uint preset, bool curvature) {
    return curvature ? CrtCurvedAxis(y, x, height, width, CrtPresets[preset].curvatureY) : y;
}

// The vignette's factor on the light at picture position (x, y): 1 at the
// centre, falling off with the squared distance from it.
CRT_FILTER_FUNCTION float CrtVignette(float x, float y, float width, float height) {
    const float centredX = CrtCentred(x, width);
    const float centredY = CrtCentred(y, height);
    const float distanceSquared = (centredX * centredX + centredY * centredY) * 0.5f;
    return CrtMax(1.0f - CrtVignetteStrength * distanceSquared, 0.0f);
}

// How much of the output pixel at picture position (x, y) the picture covers:
// 1 inside, 0 outside the rectangle with rounded corners, anti-aliased over
// about one output pixel. Picture positions are close enough to output pixels
// near the edge for the border's width.
CRT_FILTER_FUNCTION float CrtPictureCoverage(float x, float y, float width, float height) {
    const float radius = CrtCornerRadius * CrtMin(width, height);
    // Signed distance to the rounded rectangle, negative inside.
    const float cornerX = abs(x - width * 0.5f) - (width * 0.5f - radius);
    const float cornerY = abs(y - height * 0.5f) - (height * 0.5f - radius);
    const float outsideX = CrtMax(cornerX, 0.0f);
    const float outsideY = CrtMax(cornerY, 0.0f);
    const float distance = sqrt(outsideX * outsideX + outsideY * outsideY) + CrtMin(CrtMax(cornerX, cornerY), 0.0f) - radius;
    return CrtClamp01(0.5f - distance);
}

#ifndef __HLSL_VERSION
} // namespace n64_video::crt
#endif

#undef CRT_FILTER_CONSTANT
#undef CRT_FILTER_FUNCTION
