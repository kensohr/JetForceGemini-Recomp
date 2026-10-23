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
  RDP writes one: the dither value (0-7) is added to each 8-bit channel, the
  result is clamped and the top 5 bits are kept. This brings back the
  console's colour banding and dither grain. A 32-bit framebuffer is shown as
  rendered.
  - Pattern: magic square, the libultra default. Jet Force Gemini's draw
    modes select it (`G_CD_MAGICSQ` in its other-mode setup); its copy-mode
    draws disable dithering, which the RDP does for copy mode anyway.
- **Dither filter.** When the game sets the dither-filter bit in
  `VI_CONTROL`, each 16-bit pixel is compared with its 3x3 neighbourhood:
  per channel, it moves one step (of 255) toward each of the 8 neighbours
  whose 5-bit value differs from its own. This smooths the dither grain and
  restores some of the low bits the 16-bit framebuffer dropped. With the bit
  clear the grain shows unfiltered.
- **Horizontal resampling.** Each framebuffer row is resampled to the active
  scanline width (`hEnd - hStart` from `VI_H_START`, 640 samples in a normal
  NTSC mode). Sample `i` reads the framebuffer at
  `(xOffset + i * xScale) / 1024` (`VI_X_SCALE`, 2.10 fixed point) and blends
  the two neighbouring pixels with the top 5 bits of the fraction, as the VI
  does. In AA mode 3 it repeats the nearest pixel instead.
- **Gamma**, from `VI_CONTROL`, as RT64 already applied it.

Without the dither filter, a 5-bit channel is shown as its 8-bit value with
the low 3 bits clear, as on the console, so full white is 248 of 255.

All of this runs in a native-resolution VI pre-pass, one invocation per output
sample of each framebuffer row (2.10 fixed-point VI arithmetic in integers),
before RT64's VI pass applies gamma and scales the image to the window. The
dither grain is aligned to framebuffer pixels, and the window filter only
blends already-filtered samples. Placement and aspect ratio stay as RT64 sets
them: `hStart` and `vStart` don't move the image, and there's no overscan
border or crop.

Interpolated frames go through the same VI pass, so they get the same
treatment.

## Known approximations

- **Dithering happens once per presented frame.** The console dithers on
  every RDP blend into the 16-bit framebuffer, with the pattern each draw
  selects. The pre-pass dithers the finished frame once with one fixed
  pattern. Opaque surfaces look the same; layered transparent surfaces (fog,
  explosions, water) carry less accumulated dither noise than on hardware.
- **No VI anti-aliasing or divot filter.** Both need each pixel's coverage,
  which RT64 doesn't produce. Polygon edges therefore look sharper than on
  the console.
- **The dither filter runs on every pixel.** The console runs it only on fully
  covered pixels and anti-aliases edge pixels instead; without coverage every
  pixel counts as fully covered.
- **No vertical resampling.** Rows are scaled to the window as RT64 does; the
  VI's `VI_Y_SCALE` interpolation and interlacing aren't emulated.
- **No gamma dither.** The VI's random low-bit noise (`VI_CONTROL` gamma
  dither) isn't added.
- **16-bit framebuffers only.** A 32-bit framebuffer is shown as rendered,
  without the dither filter or resampling.
- Pixels outside the framebuffer repeat its edge pixel; the console reads
  whatever memory lies next to it.

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
