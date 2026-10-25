//
// Video core CRT filter: draws a native-resolution picture to an output
// rectangle as a CRT would show it. A standalone include: it knows neither
// RT64 nor the N64, so any renderer can call CrtFilter from a pixel shader
// that covers the picture's rectangle. The per-channel arithmetic and the
// CRT preset table live in crt_filter.h, which the C++ tests share.
//
// Colour order: the source holds the signal the display receives; the video
// connection's luma/chroma blur runs on that signal (in gamma space, in YIQ),
// it is decoded to linear light with the preset's CRT gamma, each scanline is
// filtered along its row, scanlines and mask are applied in linear light, and
// the result is encoded as sRGB for a UNORM swapchain.
//
// Our own HLSL after Timothy Lottes' public-domain CRT approach (ADR 0004).
//

#ifndef CRT_FILTER_HLSLI
#define CRT_FILTER_HLSLI

#include "crt_filter.h"

// One source pixel's signal: the source with the remaining signal gamma
// applied. Columns outside the picture repeat its edge.
CrtTriple CrtFetchSignal(Texture2D<float4> source, int2 sourceSize, int x, int row, float signalGamma) {
    const int column = clamp(x, 0, sourceSize.x - 1);
    const float3 signal = pow(saturate(source.Load(int3(column, row, 0)).rgb), signalGamma);
    return CrtMakeTriple(signal.r, signal.g, signal.b);
}

// A scanline's colour at `sourceX` (in source pixels from the left edge), in
// linear light. Each source row's signal is first blurred as the preset's
// video connection does (luma and chroma apart), then decoded; the scanline
// is a normalised horizontal Gaussian of the nearest decoded pixels. A
// scanline of an interlaced picture shows the average of its row pair.
// Scanlines outside the picture are black.
float3 CrtRowColor(Texture2D<float4> source, int2 sourceSize, float sourceX, int scanline, uint rowsPerScanline, float signalGamma, uint preset, CrtSignalKernels kernels) {
    const int scanlineCount = sourceSize.y / int(rowsPerScanline);
    if ((scanline < 0) || (scanline >= scanlineCount)) {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const int nearest = int(floor(sourceX));
    const float offset = sourceX - (float(nearest) + 0.5f);
    const int windowStart = nearest - CrtRowTapRadius - CrtSignalKernelRadius;
    float3 light[2 * CrtRowTapRadius + 1];
    [unroll] for (int tap = 0; tap <= 2 * CrtRowTapRadius; tap++) {
        light[tap] = float3(0.0f, 0.0f, 0.0f);
    }

    for (uint row = 0; row < rowsPerScanline; row++) {
        const int sourceRow = scanline * int(rowsPerScanline) + int(row);
        CrtTriple window[CrtSignalWindowTaps];
        [unroll] for (uint i = 0; i < CrtSignalWindowTaps; i++) {
            window[i] = CrtFetchSignal(source, sourceSize, windowStart + int(i), sourceRow, signalGamma);
        }
        [unroll] for (int tap = 0; tap <= 2 * CrtRowTapRadius; tap++) {
            const CrtTriple signal = CrtBlurSignal(window, CrtSignalKernelRadius + tap, kernels);
            light[tap] += float3(CrtDecode(signal.x, preset), CrtDecode(signal.y, preset), CrtDecode(signal.z, preset));
        }
    }

    float3 color = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;
    [unroll] for (int tap = -CrtRowTapRadius; tap <= CrtRowTapRadius; tap++) {
        const float weight = CrtPixelWeight(offset - float(tap), preset);
        color += weight * light[tap + CrtRowTapRadius];
        totalWeight += weight;
    }

    return color / (totalWeight * float(rowsPerScanline));
}

// The CRT filter's output for one pixel of the output rectangle.
// - source, sourceSize: the native-resolution picture and its size in pixels.
// - outputPosition: the pixel's centre, in output pixels from the rectangle's
//   top-left corner; outputSize: the rectangle's size in output pixels.
// - preset: a CRT preset id from crt_filter.h (not CrtPresetOff).
// - curvature: screen curvature. The picture bends by the preset's curvature
//   and fits inside the rectangle, black outside, with a vignette and rounded
//   corners; the mask stays on whole output pixels. Off leaves the picture
//   flat and fills the rectangle.
// - signalGamma: an exponent the source still needs before it is the
//   display's signal (a console's gamma stage, for example); 1 for none.
// Returns the sRGB-encoded colour, alpha 1.
float4 CrtFilter(Texture2D<float4> source, int2 sourceSize, float2 outputPosition, float2 outputSize, uint preset, bool curvature, float signalGamma) {
    const uint rowsPerScanline = CrtRowsPerScanline(uint(sourceSize.y));
    const uint scanlineCount = CrtScanlineCount(uint(sourceSize.y));
    // Where the picture is sampled: the output position, warped by curvature.
    const float2 picturePosition = float2(
        CrtCurvedX(outputPosition.x, outputPosition.y, outputSize.x, outputSize.y, preset, curvature),
        CrtCurvedY(outputPosition.x, outputPosition.y, outputSize.x, outputSize.y, preset, curvature));
    // The vignette and rounded corners, in linear light before the mask.
    float shade = 1.0f;
    if (curvature) {
        shade = CrtVignette(picturePosition.x, picturePosition.y, outputSize.x, outputSize.y) *
            CrtPictureCoverage(picturePosition.x, picturePosition.y, outputSize.x, outputSize.y);
        if (shade <= 0.0f) {
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
    }

    const float sourceX = picturePosition.x / outputSize.x * float(sourceSize.x);
    const float scanlinePosition = CrtScanlinePosition(picturePosition.y, outputSize.y, scanlineCount);
    const int scanline = int(floor(scanlinePosition));
    const float distance = scanlinePosition - (float(scanline) + 0.5f);

    // The signal blur's kernels depend only on the preset.
    const CrtSignalKernels kernels = CrtMakeSignalKernels(preset);
    const float3 above = shade * CrtRowColor(source, sourceSize, sourceX, scanline - 1, rowsPerScanline, signalGamma, preset, kernels);
    const float3 centre = shade * CrtRowColor(source, sourceSize, sourceX, scanline, rowsPerScanline, signalGamma, preset, kernels);
    const float3 below = shade * CrtRowColor(source, sourceSize, sourceX, scanline + 1, rowsPerScanline, signalGamma, preset, kernels);

    // The mask follows whole output pixels, not the warp (a warped mask would
    // land between pixels and beat with them).
    const uint column = uint(max(floor(outputPosition.x), 0.0f));
    const uint row = uint(max(floor(outputPosition.y), 0.0f));
    const float3 light = float3(
        CrtChannelAt(above.r, centre.r, below.r, distance, column, row, 0, preset),
        CrtChannelAt(above.g, centre.g, below.g, distance, column, row, 1, preset),
        CrtChannelAt(above.b, centre.b, below.b, distance, column, row, 2, preset));
    return float4(CrtEncodeSrgb(light.r), CrtEncodeSrgb(light.g), CrtEncodeSrgb(light.b), 1.0f);
}

#endif // CRT_FILTER_HLSLI
