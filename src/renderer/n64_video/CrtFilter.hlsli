//
// Video core CRT filter: draws a native-resolution picture to an output
// rectangle as a CRT would show it. A standalone include: it knows neither
// RT64 nor the N64, so any renderer can call CrtFilter from a pixel shader
// that covers the picture's rectangle. The per-channel arithmetic and the
// CRT preset table live in crt_filter.h, which the C++ tests share.
//
// Colour order: the source holds the signal the display receives; it is
// decoded to linear light with the preset's CRT gamma, each scanline is
// filtered along its row, scanlines and mask are applied in linear light, and
// the result is encoded as sRGB for a UNORM swapchain.
//
// Our own HLSL after Timothy Lottes' public-domain CRT approach (ADR 0004).
//

#ifndef CRT_FILTER_HLSLI
#define CRT_FILTER_HLSLI

#include "crt_filter.h"

// Source pixels on each side of the nearest one that a scanline's horizontal
// beam reads.
static const int CrtRowTapRadius = 2;

// One source pixel of a scanline in linear light. A scanline of an interlaced
// picture shows the average of its row pair. Columns outside the picture
// repeat its edge; scanlines outside it are black.
float3 CrtFetch(Texture2D<float4> source, int2 sourceSize, int x, int scanline, uint rowsPerScanline, float signalGamma, uint preset) {
    const int scanlineCount = sourceSize.y / int(rowsPerScanline);
    if ((scanline < 0) || (scanline >= scanlineCount)) {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const int column = clamp(x, 0, sourceSize.x - 1);
    float3 light = float3(0.0f, 0.0f, 0.0f);
    for (uint row = 0; row < rowsPerScanline; row++) {
        const float3 signal = pow(saturate(source.Load(int3(column, scanline * int(rowsPerScanline) + int(row), 0)).rgb), signalGamma);
        light += float3(CrtDecode(signal.r, preset), CrtDecode(signal.g, preset), CrtDecode(signal.b, preset));
    }

    return light / float(rowsPerScanline);
}

// A scanline's colour at `sourceX` (in source pixels from the left edge): a
// normalised horizontal Gaussian of the nearest source pixels.
float3 CrtRowColor(Texture2D<float4> source, int2 sourceSize, float sourceX, int scanline, uint rowsPerScanline, float signalGamma, uint preset) {
    const int nearest = int(floor(sourceX));
    const float offset = sourceX - (float(nearest) + 0.5f);
    float3 color = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;
    [unroll] for (int tap = -CrtRowTapRadius; tap <= CrtRowTapRadius; tap++) {
        const float weight = CrtPixelWeight(offset - float(tap), preset);
        color += weight * CrtFetch(source, sourceSize, nearest + tap, scanline, rowsPerScanline, signalGamma, preset);
        totalWeight += weight;
    }

    return color / totalWeight;
}

// The CRT filter's output for one pixel of the output rectangle.
// - source, sourceSize: the native-resolution picture and its size in pixels.
// - outputPosition: the pixel's centre, in output pixels from the rectangle's
//   top-left corner; outputSize: the rectangle's size in output pixels.
// - preset: a CRT preset id from crt_filter.h (not CrtPresetOff).
// - curvature: screen curvature; reserved, not applied yet.
// - signalGamma: an exponent the source still needs before it is the
//   display's signal (a console's gamma stage, for example); 1 for none.
// Returns the sRGB-encoded colour, alpha 1.
float4 CrtFilter(Texture2D<float4> source, int2 sourceSize, float2 outputPosition, float2 outputSize, uint preset, bool curvature, float signalGamma) {
    const uint rowsPerScanline = CrtRowsPerScanline(uint(sourceSize.y));
    const uint scanlineCount = CrtScanlineCount(uint(sourceSize.y));
    const float sourceX = outputPosition.x / outputSize.x * float(sourceSize.x);
    const float scanlinePosition = CrtScanlinePosition(outputPosition.y, outputSize.y, scanlineCount);
    const int scanline = int(floor(scanlinePosition));
    const float distance = scanlinePosition - (float(scanline) + 0.5f);

    const float3 above = CrtRowColor(source, sourceSize, sourceX, scanline - 1, rowsPerScanline, signalGamma, preset);
    const float3 centre = CrtRowColor(source, sourceSize, sourceX, scanline, rowsPerScanline, signalGamma, preset);
    const float3 below = CrtRowColor(source, sourceSize, sourceX, scanline + 1, rowsPerScanline, signalGamma, preset);

    // The mask follows whole output pixels.
    const uint column = uint(max(floor(outputPosition.x), 0.0f));
    const float3 light = float3(
        CrtChannel(above.r, centre.r, below.r, distance, column, 0, preset),
        CrtChannel(above.g, centre.g, below.g, distance, column, 1, preset),
        CrtChannel(above.b, centre.b, below.b, distance, column, 2, preset));
    return float4(CrtEncodeSrgb(light.r), CrtEncodeSrgb(light.g), CrtEncodeSrgb(light.b), 1.0f);
}

#endif // CRT_FILTER_HLSLI
