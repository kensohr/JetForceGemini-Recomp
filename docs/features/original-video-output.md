# Original video output

**Video output: Enhanced / Original** on the Display tab of the launcher's
Video settings shows the game as the console sent it to the TV. Enhanced is
the default and the port's usual window-scaled image. Like the other
rendering settings, a change applies on the next game launch.

## What Original does

- **Native resolution.** RT64 renders at 1x (320x240) and fills the window
  height with RT64's pixel-scaling filter: pixels stay sharp and only their
  edges blend over about one screen pixel. The aspect ratio still follows the
  game.
- **16-bit colour with dither.** When the game's `VI_CONTROL` selects a
  16-bit framebuffer, each presented frame is reduced to RGBA5551 the way the
  RDP writes one: an 8-bit channel rounds up to the next 5-bit step when its
  low 3 bits exceed the dither value (0-7), clamped at 31, and otherwise
  keeps its top 5 bits. This brings back the console's colour banding and
  dither grain. A 32-bit framebuffer keeps its 8-bit colour; the dither
  filter and resampling below still apply to it.
  - Pattern: magic square, the libultra default. Jet Force Gemini's draw
    modes select it (`G_CD_MAGICSQ` in its other-mode setup); its copy-mode
    draws disable dithering, which the RDP does for copy mode anyway.
- **Dither filter.** When the game sets the dither-filter bit in
  `VI_CONTROL`, each pixel is compared with its 3x3 neighbourhood: per
  channel, it moves one step (of 255) toward each of the 8 neighbours whose
  top 5 bits differ from its own. On a 16-bit framebuffer this smooths the
  dither grain and restores some of the low bits it dropped. With the bit
  clear the grain shows unfiltered.
- **Horizontal resampling.** Each framebuffer row is resampled to the active
  scanline width (`hEnd - hStart` from `VI_H_START`, 640 samples in a normal
  NTSC mode). Sample `i` reads the framebuffer at
  `(xOffset + i * xScale) / 1024` (`VI_X_SCALE`, 2.10 fixed point) and blends
  the two neighbouring pixels with the top 5 bits of the fraction, as the VI
  does. In AA mode 3 it repeats the nearest pixel instead.
- **Gamma**, from `VI_CONTROL`, as RT64 already applied it.
- **Edges anti-aliased with MSAA, an approximation of VI anti-aliasing.**
  RT64 renders with 8x MSAA (4x if the device lacks 8x, none if it lacks
  both or custom sample locations) and the pre-pass reads each pixel's
  averaged colour, so polygon silhouettes come out softened. The game log
  names the count in use (`Original video output: MSAA 8x in use`, or a
  warning on a fallback).

Without the dither filter, a 5-bit channel is shown as its 8-bit value with
the low 3 bits clear, as on the console, so full white is 248 of 255.

All of this runs in a native-resolution VI pre-pass, one invocation per output
sample of each framebuffer row (2.10 fixed-point VI arithmetic in integers),
before RT64's VI pass applies gamma and scales the image to the window. The
dither grain is aligned to framebuffer pixels, and the window filter only
blends already-filtered samples. Placement and aspect ratio stay as RT64 sets
them: `hStart` and `vStart` don't move the image, and there's no overscan
border or crop.

Interpolated frames are rendered with the same MSAA (RT64 keeps one
multisampling setup for all render targets) and go through the same VI pass,
so they get the same treatment.

## Known approximations

- **Dithering happens once per presented frame.** The console dithers on
  every RDP blend into the 16-bit framebuffer, with the pattern each draw
  selects. The pre-pass dithers the finished frame once with one fixed
  pattern. Opaque surfaces look the same; layered transparent surfaces (fog,
  explosions, water) carry less accumulated dither noise than on hardware.
- **MSAA instead of VI anti-aliasing** (`msaa`, the default; `coverage`
  below emulates it). The console's VI blends only edge pixels (partial
  coverage) with a background it reconstructs from the neighbouring pixels.
  RT64 doesn't produce coverage, so MSAA stands in:
  - MSAA softens every edge, including draws the console keeps sharp: level
    geometry on levels that don't anti-alias, the HUD and decals.
  - It blends with the true background instead of the VI's reconstruction,
    so VI anti-aliasing's own artefacts don't appear.
  - Cut-out texture edges (foliage) stay sharp; on the console they are
    anti-aliased through alpha-to-coverage.
  - The dither filter still runs on edge pixels.
  - The VI anti-aliasing mode in `VI_CONTROL` is ignored; Jet Force Gemini
    always runs mode 0.
- **No divot filter** (`msaa` and `off`). It repairs the one-pixel gaps VI
  anti-aliasing leaves where edges meet; MSAA leaves none.
- **The dither filter runs on every pixel** (`msaa` and `off`). The console
  runs it only on fully covered pixels and anti-aliases edge pixels instead;
  without coverage every pixel counts as fully covered.
- **No vertical resampling.** Rows are scaled to the window as RT64 does; the
  VI's `VI_Y_SCALE` interpolation and interlacing aren't emulated.
- **No gamma dither.** The VI's random low-bit noise (`VI_CONTROL` gamma
  dither) isn't added.
- Pixels outside the framebuffer repeat its edge pixel; the console reads
  whatever memory lies next to it.

## Temporary developer switch: `JFG_VI_AA`

While two ways of approximating VI anti-aliasing are compared, the
environment variable `JFG_VI_AA` picks one at game start, under Original
only. It is developer-only, not a launcher setting, and is removed once one
approach is chosen.

- unset, empty or `msaa`: MSAA, as above (the default).
- `off`: no anti-aliasing; edges are hard and stair-stepped.
- `coverage`: VI anti-aliasing and divot filter from estimated coverage,
  below.
- Anything else logs a warning and uses `msaa`.

### `coverage`: VI anti-aliasing from estimated coverage

The console's RDP stores each pixel's coverage (how many of its 8
sub-samples the surface covers) next to its colour, and the VI uses it. This
option estimates that coverage from RT64's MSAA samples and runs the VI's own
filters on it. The game log says `VI anti-aliasing and divot filter from
estimated coverage (JFG_VI_AA=coverage)`, then the MSAA count, and at debug
level `coverage estimate running` once the first frame is estimated.

- **Sample positions.** RT64 renders with 8x MSAA at the RDP's 8 sample
  positions (4 sub-scanlines, x 0 and 4/8 on rows 0 and 2, 2/8 and 6/8 on
  rows 1 and 3). Because RT64 draws the RDP's pixel corner at the rendered
  pixel's centre, half of each RDP pixel's samples land in the rendered
  pixels to its right and below; the estimate gathers them from there.
- **Render mode per sample.** Every draw records in the colour target's
  alpha, per covered sample, what the RDP would do with coverage: partial
  coverage for anti-aliased draws (`AA_EN` with `CVG_DST_CLAMP`), full for
  `CVG_DST_FULL`, copy, fill and non-anti-aliased draws, the coverage under it
  for `CVG_DST_SAVE` (clouds, particles), and coverage times alpha under
  `CVG_X_ALPHA` (cut-outs such as foliage). RT64's own coverage bits, which it
  writes back to RDRAM, stay below the record.
- **Coverage and colour.** After each framebuffer pair, a compute pass finds
  each pixel's front surface: the nearest sample by depth plus every sample
  within 8 times the local depth slope of it, the RDP's own tolerance for
  "the same surface". An anti-aliased front covers the share of samples it
  covers; its colour is the average of its samples only, never the
  background, because the VI reconstructs that. Any other front is drawn
  whole by its top-left sample, as the RDP draws a pixel without
  anti-aliasing. Interpolated frames copy the estimate of the frame they're
  resolved from.
- **VI pre-pass**, in the VI's order: 16-bit reduction of the front colour;
  per pixel, VI anti-aliasing when it's partially covered, else the dither
  filter (so the dither filter no longer runs on edge pixels); the divot
  filter when `VI_CONTROL` bit 4 is set; resampling; gamma.
  - VI anti-aliasing: from the 6 neighbours at (-1,-1), (+1,-1), (-2,0),
    (+2,0), (-1,+1), (+1,+1) that are fully covered, per channel the
    penultimate maximum and minimum (the centre counted twice) stand for the
    background, and the pixel moves toward it by its missing coverage:
    `c + (((min + max - 2c) * (7 - coverage) + 4) >> 3)`.
  - Divot: where the pixel or its left or right neighbour is partially
    covered, each channel becomes the median of the three.
  - AA modes 2 and 3 treat every pixel as fully covered.

Known deviations of `coverage`:

- **Coverage from depth, not per primitive.** The RDP accumulates coverage
  primitive by primitive, blending surfaces within its depth tolerance; the
  estimate groups the finished samples by depth once. The tolerance uses the
  depth slope of the neighbouring pixels (per axis the smaller difference, so
  a silhouette on one side doesn't widen it), not each primitive's own. Thin
  geometry with another surface on both sides (one-pixel railings) gets a
  wide tolerance and counts as fully covered, softened only by the averaged
  colour.
- **`CVG_DST_SAVE`** keeps the coverage recorded under the draw, which is
  what the RDP stores; the colour the draw blends in still enters the front
  colour.
- **`CVG_DST_WRAP`** (translucent anti-aliased surfaces, anti-aliased decals)
  keeps the coverage under it too. The RDP adds coverage modulo 8, so on the
  console a translucent edge drawn over a fully covered surface gets partial
  coverage and is anti-aliased; here it isn't.
- **Decals and non-anti-aliased draws** are drawn whole when they cover the
  top-left sample; their edge pixels take that sample's colour.
- **4x fallback.** Without 8x MSAA, RT64 uses 4 of the RDP's positions, one
  per sub-scanline ((0,0), (6,2), (4,4), (2,6) in eighths), and each sample
  counts as two eighths. Coverage then has 4 steps instead of 8.
- **Pairs without a depth target** (2D passes) reuse the front surface the
  frame's last pair with depth found; if there is none, every sample counts
  as front and coverage comes from the draws' render modes only. This is
  logged once at debug level.
- **Where the estimate can't run** (MSAA unavailable, a resolution or aspect
  scale other than 1, downsampling), the frame shows the averaged colour at
  full coverage, as `msaa` does; each cause is logged once at debug level.
- **RDRAM coverage bits.** The colour written back to RDRAM, and a
  framebuffer read as a 16-bit texture, still carry RT64's coverage bit, but
  on edge pixels it is computed from the average of the samples' records, so
  it can differ from `msaa`. A 32-bit framebuffer's alpha byte carries the
  record (Jet Force Gemini draws to a 16-bit framebuffer).

## For other RT64 projects

The emulation lives in the video core, `src/renderer/n64_video/`, which has no
game-specific code and is configured only through `n64_video::VideoOptions`
and the VI registers. `cmake/rt64_n64_video.cmake` is its RT64 patch. The core decides what
the pre-pass does (`colour_reduction.hpp`, `vi_filtering.hpp`); the patch adds
the pre-pass to `VideoInterfacePS.hlsl`, registers it in RT64's
`CMakeLists.txt` and `rt64_shader_library.*`, extends `VideoInterfaceCB`, and
runs it from `VIRenderer::render`. The present queue passes the swap-chain
framebuffer so the VI pass can rebind it after the pre-pass. Every patched
input is hash-checked against the pinned RT64.

For MSAA, `apply_to_rt64` requests 8x when `VideoOptions::anti_aliasing` is
`msaa`. RT64 only turns an unsupported count off, so a project lowers it with
`limit_msaa_to_device` (`msaa.hpp` holds the fallback) once the device
exists: Jet Force Gemini does this from RT64's render init hook, which setup
calls before its own MSAA check (`src/renderer/rt64_msaa_support.hpp`).

The `coverage` option is a second, separate patch,
`cmake/rt64_vi_coverage.cmake`, applied after the VI-pass patch, so a project
can take the MSAA option alone. Its decisions and reference arithmetic are in
the core (`coverage_estimate.hpp`, `vi_anti_aliasing.hpp`, and the AA and
divot bits in `vi_filtering.hpp`); the files it adds to RT64 (the estimate
compute shader, the raster shader's record, the per-target estimate) are in
`src/renderer/n64_video/rt64_patch/`. The patch extends the raster shader,
its frame parameters and sample positions, `RenderTarget`, the shader
library, the workload queue (one estimate per framebuffer pair, copied to
interpolation targets), the present queue and the VI pre-pass. A project
reports the estimate's events by setting `n64_video::coverage_log_sink()`.
