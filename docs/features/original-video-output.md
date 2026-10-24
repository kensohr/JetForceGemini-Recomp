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
- **MSAA instead of VI anti-aliasing.** The console's VI blends only edge
  pixels (partial coverage) with a background it reconstructs from the
  neighbouring pixels. RT64 doesn't produce coverage, so MSAA stands in:
  - MSAA softens every edge, including draws the console keeps sharp: level
    geometry on levels that don't anti-alias, the HUD and decals.
  - It blends with the true background instead of the VI's reconstruction,
    so VI anti-aliasing's own artefacts don't appear.
  - Cut-out texture edges (foliage) stay sharp; on the console they are
    anti-aliased through alpha-to-coverage.
  - The dither filter still runs on edge pixels.
  - The VI anti-aliasing mode in `VI_CONTROL` is ignored; Jet Force Gemini
    always runs mode 0.
- **No divot filter.** It repairs the one-pixel gaps VI anti-aliasing leaves
  where edges meet; MSAA leaves none.
- **The dither filter runs on every pixel.** The console runs it only on fully
  covered pixels and anti-aliases edge pixels instead; without coverage every
  pixel counts as fully covered.
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
- Anything else logs a warning and uses `msaa`.

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
