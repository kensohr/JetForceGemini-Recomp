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
  - The reduction runs in a native-resolution VI pre-pass, one invocation per
    framebuffer pixel, before RT64's VI pass applies gamma and scales the
    image to the window. The dither grain is aligned to framebuffer pixels,
    and the window filter only blends already-reduced pixels.
  - Interpolated frames go through the same VI pass, so they get the same
    treatment.

## Known approximations

- **Dithering happens once per presented frame.** The console dithers on
  every RDP blend into the 16-bit framebuffer, with the pattern each draw
  selects. The pre-pass dithers the finished frame once with one fixed
  pattern. Opaque surfaces look the same; layered transparent surfaces (fog,
  explosions, water) carry less accumulated dither noise than on hardware.
- Reduced channels are expanded back to 8 bits by repeating their top bits,
  as RT64 does for 16-bit framebuffers, so full white stays white. The VI's
  dither filter, which recovers the lost low bits on hardware, is not
  applied yet.

## For other RT64 projects

The emulation lives in the video core, `src/renderer/n64_video/`, which has no
game-specific code and is configured only through `n64_video::VideoOptions`
and the VI registers. `cmake/rt64_n64_video.cmake` is its RT64 patch: it adds
the pre-pass to `VideoInterfacePS.hlsl`, registers it in RT64's
`CMakeLists.txt` and `rt64_shader_library.*`, extends `VideoInterfaceCB`, and
runs it from `VIRenderer::render`. The present queue passes the swap-chain
framebuffer so the VI pass can rebind it after the pre-pass. Every patched
input is hash-checked against the pinned RT64.
