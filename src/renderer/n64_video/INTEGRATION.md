# Video core integration guide

This folder is a **video core** for RT64-based N64 recompilations
(RecompFrontend projects such as Zelda64Recomp or BanjoRecomp). It adds two
things to RT64's video-output (VI) pass:

- **Original video output**: the game as the console sent it to the TV, at
  native resolution with the VI's own filtering.
- **CRT filter**: on top of Original, a simulation of the display the console
  was connected to (RGB monitor, S-Video TV, Composite TV), with optional
  screen curvature.

The core has no game-specific types, addresses or names. A project configures
it through one options struct, `n64_video::VideoOptions`, and the VI registers
the game writes.

## Licence and provenance

- The code is **AI-assisted**: written with an AI coding assistant and
  reviewed by the developer.
- It is MIT-licensed, under the `LICENSE` at the root of the repository it
  comes from. Keep that copyright notice when you copy it.
- **Don't submit it to RT64**, whole or in part, as a pull request or patch.
  RT64's `CONTRIBUTING.md` rejects AI-generated contributions. That is why the
  core lives outside RT64 as a patch, and why a project applies that patch
  itself.
- The CRT shaders are original HLSL that follow Timothy Lottes'
  public-domain CRT approach. They contain no GPL shader code (crt-royale,
  crt-geom, crt-guest-advanced, libretro NTSC shaders). Keep it that way:
  pasting a GPL shader in would make the core impossible to combine with MIT
  code.

## What it emulates

Everything below runs only under Original video output, follows the bits the
game writes to `VI_CONTROL`, `VI_H_START` and `VI_X_SCALE`, and applies to
every presented frame, interpolated ones included. The reference arithmetic is
in the C++ headers named in brackets; the shaders repeat it.

### Original video output

- **Native resolution.** RT64 renders at 1× (320×240 for most games). RT64's
  own VI pass still places the picture, keeps the game's aspect ratio and
  scales it to the window with its pixel-scaling filter (sharp pixels, edges
  blended over about one screen pixel).
- **16-bit colour with dither** (`colour_reduction.hpp`). When `VI_CONTROL`
  selects a 16-bit framebuffer, each presented frame is reduced to RGBA5551
  the way the RDP writes one, with the magic-square pattern. A 32-bit
  framebuffer keeps its 8-bit colour.
- **VI anti-aliasing and the divot filter, from estimated coverage**
  (`coverage_estimate.hpp`, `vi_anti_aliasing.hpp`, `msaa.hpp`). RT64 doesn't
  produce the RDP's per-pixel coverage, so the core estimates it:
  - RT64 renders with 8× MSAA at the RDP's 8 sample positions (4× if the
    device lacks 8×, none if it lacks both or custom sample locations).
  - The raster shader marks the samples of anti-aliased draws (`AA_EN` with
    `CVG_DST_CLAMP`). Resolves take the mark off again, so RT64's own
    coverage bits and RDRAM writebacks are unchanged.
  - After each framebuffer pair a compute pass finds each pixel's front
    surface by depth, its coverage and its colour.
  - The VI pre-pass then runs the VI's anti-aliasing on partially covered
    pixels (AA modes 0 and 1) and the divot filter (`VI_CONTROL` bit 4).
- **Dither filter** (`vi_filtering.hpp`) on fully covered pixels, when
  `VI_CONTROL` bit 16 is set.
- **Horizontal resampling** (`vi_filtering.hpp`). Each row is resampled to the
  active scanline width (`VI_H_START`), with `VI_X_SCALE`'s 2.10 fixed-point
  step and offset, interpolating as the VI does; AA mode 3 replicates.
- **Gamma dither** (`gamma_dither.hpp`). With `VI_CONTROL` bit 2 set and bit 3
  clear, each output sample gets a random 0 or 1 per 8-bit channel, after
  resampling.
- **Gamma**, as RT64 already applies it.

All of this runs in a native-resolution VI pre-pass (one row per framebuffer
row, one column per scanline sample) before RT64's VI pass.

### CRT filter

`crt_filter.h` (the preset table and arithmetic, shared by C++ and HLSL) and
`CrtFilter.hlsli` (the standalone shader include). With a CRT preset, the VI
pass's final draw runs the CRT filter on the pre-pass output instead of RT64's
pixel-scaling filter. It fills the same picture rectangle, black outside.

- **Colour order.** VI gamma, then the video connection's luma and chroma blur
  in gamma space (YIQ, horizontal Gaussians), a decode to linear light with a
  CRT gamma of 2.4, scanlines and mask in linear light, and an sRGB encode for
  the UNORM swapchain.
- **Scanlines.** One per framebuffer row up to 288 rows. Taller (interlaced)
  frames get one per row pair, with no interlace flicker.
- **Presets:**
  - *RGB monitor*: no signal blur, pronounced scanlines, an aperture grille
    with one RGB triad per three output columns.
  - *S-Video TV*: slight luma blur, moderate chroma blur, faint scanlines, a
    slot mask.
  - *Composite TV*: as S-Video, with wider luma and chroma blur and a small
    rightward chroma shift, which gives colour bleed.
- **Screen curvature** (optional). Lottes' warp with per-preset strengths
  (RGB monitor is curved horizontally only), a vignette and rounded corners.
  The curved picture fits inside the rectangle and the mask stays aligned to
  output pixels.

`CrtFilter.hlsli` knows neither RT64 nor the N64. Its inputs are a source
texture and its size, an output position and size, a preset id, the curvature
flag and a remaining signal gamma, so other renderers can call it.

### What it doesn't emulate

- **Per-draw RDP dithering.** The finished frame is dithered once, with one
  fixed pattern; layered transparent surfaces carry less dither noise than on
  hardware.
- **Exact RDP coverage.** Coverage is estimated from the finished MSAA
  samples and each draw's render mode, not accumulated primitive by
  primitive. `CVG_DST_WRAP` doesn't wrap modulo 8, thin geometry between two
  surfaces can count as fully covered, and the 4× fallback has 4 coverage
  steps instead of 8.
- **Coverage at other scales.** With a resolution or aspect scale other than
  1, or with downsampling, the estimate can't run. The frame then falls back
  to RT64's averaged MSAA colour at full coverage, with a warning.
- **Gamma dither with gamma on** (`VI_CONTROL` bits 2 and 3 both set). No
  noise is added, with a warning. The noise also comes from a hash, not the
  VI's own random generator.
- **Vertical resampling and interlacing** (`VI_Y_SCALE`). Rows are scaled to
  the window as RT64 does.
- **Picture position and overscan.** `hStart` and `vStart` don't move the
  picture, and there's no overscan border or crop.
- **Pixels outside the framebuffer** repeat its edge pixel. The console reads
  whatever memory lies next to it.
- **CRT filter:** no dot crawl, no interlace flicker, no per-parameter
  settings, no bezel or fill-and-crop picture fit. Under Enhanced video output
  (RT64's usual scaled rendering) the CRT filter doesn't run.

## Apply the RT64 patch

The patch is `rt64_n64_video.patch` in this folder. It changes 17 RT64 files
and adds 7. It holds the same changes as the CMake patch script this
repository builds with, as a diff against the RT64 commit this repository pins.

Checked against:

| RT64 commit | `git apply --check` | Builds (Windows, MSVC, D3D12 and Vulkan backends and shaders) |
|---|---|---|
| `5473732a822a4423b5696e7cb18fecc425a59875` (pinned) | clean | yes (this repository's own build) |
| `d9817c68ffd7328cd0af383af124971489366e18` (`main`, 2026-10-23) | clean, 2 files with line offsets | yes |

1. In your RT64 checkout (a submodule, a fork or a copy):

   ```sh
   git apply --check path/to/n64_video/rt64_n64_video.patch
   git apply path/to/n64_video/rt64_n64_video.patch
   ```

   `git apply` warns about trailing whitespace on a few lines of
   `TextureResolvePS.hlsl`. That whitespace is RT64's own, so the warning is
   harmless.

2. Give RT64's target the core. The patched RT64 sources include
   `n64_video/...` headers, and one new translation unit lives in the core:

   ```cmake
   add_subdirectory(rt64)  # with RT64_STATIC ON, as RecompFrontend projects do
   # The folder that contains n64_video/, and RT64's src/render for the new unit.
   target_include_directories(rt64 PRIVATE "path/to/parent-of-n64_video" "${rt64_SOURCE_DIR}/src/render")
   target_sources(rt64 PRIVATE "path/to/n64_video/rt64_patch/rt64_coverage_estimate.cpp")
   ```

3. Your own code includes the core's headers through the same parent folder,
   e.g. `#include "n64_video/rt64_configuration.hpp"`.

The patch copies these core files into RT64: `coverage_layout.h`,
`crt_filter.h`, `CrtFilter.hlsli` and the files in `rt64_patch/` (except
`rt64_coverage_estimate.cpp`). If you change one of them in the core, change
its copy in RT64 too, or regenerate and re-apply the patch.

### Optional: a hash check for CMake projects

This repository doesn't apply the `.patch`. It patches RT64 at configure time
with `cmake/rt64_n64_video.cmake`, which never touches the RT64 checkout. The
script reads each RT64 file it changes, normalises line endings, and compares
its SHA-256 with the hash of the audited pinned file. If they differ, the
configure fails ("Pinned video core RT64 input changed"). It then replaces
each patch site, an exact text snippet that must exist or the configure
fails, writes the result to a build-private copy, and points RT64's build at
that copy. An RT64 update therefore can't be patched blindly: it fails loudly
until someone re-audits the changed files and updates their hashes. A CMake
project that wants the same protection can copy that script and
`cmake/rt64_patch_helpers.cmake`, rename the `jfg_` helper prefix and adjust
the two entry points (`jfg_n64_video_patch_source_view`,
`jfg_n64_video_patch_rt64_target`) to its own build. The `.patch` route has no
such check. `git apply --check` only tells you whether the context still
matches.

## Fill in the options

`video_options.hpp`:

| Field | Type, default | Meaning |
|---|---|---|
| `original_video_output` | `bool`, `false` | Original video output: 1× rendering, 8× MSAA for the coverage estimate, and the VI pre-pass (16-bit colour, VI anti-aliasing, dither filter, divot, resampling, gamma dither). `false` leaves RT64 as it is (Enhanced). |
| `crt_preset` | `n64_video::CrtPreset`, `off` | The CRT filter: `off`, `rgb_monitor`, `composite_tv` or `svideo_tv`. Ignored unless `original_video_output` is set. |
| `crt_curvature` | `bool`, `false` | Screen curvature, vignette and rounded corners. Ignored unless a CRT preset is active. |

Setup order:

```cpp
#include "n64_video/log_sink.hpp"
#include "n64_video/rt64_configuration.hpp"

n64_video::VideoOptions options;
options.original_video_output = settings.video_output_original;  // your setting
options.crt_preset = n64_video::CrtPreset::composite_tv;         // your setting
options.crt_curvature = settings.crt_curvature;                  // your setting

// 1. Optional: receive the core's reports (coverage statuses, MSAA
//    fallbacks, the gamma-dither warning). Set it before RT64 renders; while
//    it is unset, reports are dropped.
n64_video::log_sink() = [](n64_video::LogLevel level, const char *message) {
    my_log(level, message);
};

// 2. After your own settings are mapped onto RT64's configuration, and before
//    RT64::Application::setup, which reads it once.
n64_video::apply_to_rt64(options, app->userConfig);
```

- `apply_to_rt64` stores the options for the patched VI pass, which reads
  them on RT64's presentation thread. Don't change them after setup: a later
  change races with that thread, and a change applies on the next launch.
- Under Original it sets `resolution = Resolution::Original` and
  `antialiasing = MSAA8X`, overriding what your settings put there. It doesn't
  touch anything else.
- **Limit MSAA to the device.** RT64 only turns an unsupported MSAA count
  off, and doesn't notice a device without custom sample locations. Once the
  device exists, and before RT64 builds its multisampling, call
  `n64_video::limit_msaa_to_device(app->userConfig, support)`. It returns the
  sample count now configured. `support` holds the device's sample-count mask
  and whether it has custom sample locations. One place without patching RT64
  more is RT64's render init hook, which `Application::setup` calls between
  creating the device and its own MSAA check. If your project already uses
  RT64's render hooks (RecompFrontend projects draw their menu with the draw
  hook), chain to the previous hooks rather than replacing them. This
  repository does that in `src/renderer/rt64_msaa_support.hpp`.
- `rt64_antialiasing`, `estimates_coverage`, `crt_filter_active`,
  `crt_shader_preset` and `crt_shader_curvature` are the helpers the patch
  uses. You only need them to report the active state in your own logs or UI.

## Glue left to the project

The core decides nothing about your settings, files or UI.

- **Settings UI.** Offer Video output: Enhanced / Original, a CRT filter
  choice (Off / Composite TV / S-Video TV / RGB monitor) that is only enabled
  under Original, and a screen-curvature switch that is only enabled with a
  CRT preset. While a control is disabled, keep its stored value. Tell users
  a change applies on the next launch.
- **Persistence.** Store the three values in your own config file and fill
  the options from it at startup. Map your own enum onto `n64_video::CrtPreset`
  in your code, so a renumbering on either side can't leak through. Read
  missing or unknown values as the defaults.
- **Your "Original" resolution.** RecompFrontend projects usually offer a
  resolution setting whose "Original" choice maps to RT64's
  `Resolution::Manual` with `resolutionMultiplier = 1`, i.e. manual 1×. That
  renders the same 1× image as the core's `Resolution::Original`. But
  Original video output doesn't depend on it: `apply_to_rt64` sets 1× itself,
  so call it after your resolution mapping, and grey out or ignore your
  resolution and MSAA settings while Original video output is on. Two RT64
  settings it doesn't override must also stay neutral, or the coverage
  estimate falls back (see "What it doesn't emulate"):
  - `downsampleMultiplier` must be 1. Force it, or disable your downsampling
    setting, under Original.
  - The aspect ratio must render at a scale of 1. `AspectRatio::Original`
    does; an expanded widescreen aspect gives an x scale above 1.
- **Interpolation.** RT64's present queue runs the VI pass once per presented
  frame, so interpolated (high frame rate) frames get the same pre-pass and
  CRT filter. The patch copies each frame's coverage estimate to the
  interpolation target it's resolved into, and the gamma-dither noise
  advances once per presented frame. Interpolated frames are rendered at the
  same 1× resolution with the same MSAA. Nothing else is needed, but check
  your game's interpolation at 1× before shipping it: it is normally run at
  window resolution.
- **Overlays and screenshots.** The CRT filter runs inside the VI pass, not at
  RT64's render hook, so your menu overlay (drawn at the hook) stays unfiltered
  on top of it. A screenshot read back at the hook captures the filtered image.
- **Logging.** The core's messages are plain text. Route them through
  `log_sink()` into your logger and add your own prefix if you want one.

## Keep the patch in sync (for this repository)

The `.patch` is generated. After any change to `cmake/rt64_n64_video.cmake` or
to a core file it copies into RT64, regenerate it against the pinned RT64:

```sh
python scripts/export_n64_video_rt64_patch.py           # rewrite the .patch
python scripts/export_n64_video_rt64_patch.py --check   # fail if it's stale
```

`tests/test_n64_video_rt64_patch.py` runs the check. It finds the pinned RT64
through `JFG_RT64_ROOT` or the checkout's dependencies folder from a
development build, and skips without one.
