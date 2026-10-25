# What this fork changes

This is a community fork of
[TK22-26/JetForceGemini-Recomp](https://github.com/TK22-26/JetForceGemini-Recomp).
The original project aims to play exactly like the N64 game. This fork adds
optional features on top of it, on the
[`kensohr`](https://github.com/kensohr/JetForceGemini-Recomp/tree/kensohr)
branch. Everything else plays like the original project's version.

There are no fork releases yet. The download links in the [README](README.md)
are the original project's builds, which don't include these features. To play
them, build `kensohr` from source (see [For developers](#for-developers)).

## Features

### Camera-relative running

Under Modern Controls, the movement input picks a direction relative to the
camera and your character turns and runs that way, as in the original game.
The camera follows on a leash behind the character. Aiming still strafes
while facing the camera.

- **Turn it on:** launcher › Modern Controls › Advanced settings › Movement ›
  **Ground movement**, per player. **Run toward input (original)** is the
  default; **Face the camera (strafe)** gives the original project's Modern
  movement.
- **Status:** playable.

### Original video output

Shows the game as the console sent it to the TV: native 320x240 resolution
with sharp pixels, 16-bit colour with its dither grain, the console's
anti-aliasing and filters, and its scanline resampling.

- **Turn it on:** launcher › Video settings › Display › **Video output:
  Original**. **Enhanced** is the default. Applies on the next game launch.
- **Status:** playable.
- **More:** [docs/features/original-video-output.md](docs/features/original-video-output.md)

### CRT filter

An optional CRT look on top of Original video output: scanlines, a phosphor
mask and the blur and colour bleed of the cable, with presets for the displays
players used. Optional screen curvature bends the picture like a CRT tube.

- **Turn it on:** launcher › Video settings › Display › **CRT filter:
  Composite TV / S-Video TV / RGB monitor**, and optionally **Screen
  curvature**. Needs Video output: Original. **Off** is the default, and
  curvature is off by default. Applies on the next game launch.
- **Status:** built, not yet play-tested.
- **More:** [docs/features/original-video-output.md#crt-filter](docs/features/original-video-output.md#crt-filter)

## For developers

To build the fork, check out `kensohr` and follow the original project's
[ROM build setup](docs/development/rom-bootstrap.md).

Each feature is developed on its own branch cut from `main`, so it can be
reviewed and offered to the original project on its own. `kensohr` merges them
all; `main` mirrors the original project's `main`.

| Fork feature | Branch | Upstream status |
| --- | --- | --- |
| Camera-relative running | `modern-camera-relative-running` | not offered |
| Original video output | `original-video-output` | not offered |
| CRT filter | `crt-filter` (cut from `original-video-output`) | not offered: an enhancement, so `kensohr` only |
| Optimised from-ROM builds | `release-optimised-rom-build` | not offered |
| Fast development builds | `fast-development-builds` | not offered |

Build-only changes, which change how the game is built, not how it plays:

- **Optimised from-ROM builds**: add `--release-optimizations` to
  `scripts/build_from_rom.py` to build the runtime with the release
  optimisations of the original project's releases, so it plays at their full
  speed. Off by default. **Status:** done.
- **Fast development builds**: `scripts/dev_build.py --rom <rom>` builds in a
  fixed workspace per checkout and afterwards rebuilds only what changed: a
  one-file source change takes seconds instead of a full build. It compiles
  with Ninja, shares generated code and the pinned dependencies between
  checkouts and uses [sccache](https://github.com/mozilla/sccache) when it is
  installed. `--checkout <path>` builds another checkout, such as a feature
  worktree, without adding anything to it. `--bundle` refreshes a local
  launcher bundle for testing and warns when its launcher was built from
  other sources. `--help` lists the other options. It is a separate script: `build_from_rom.py` and
  release builds are unchanged. **Status:** done. **More:**
  [docs/development/dev-builds.md](docs/development/dev-builds.md)

**Merged upstream:** none yet.
