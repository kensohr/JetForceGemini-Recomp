# INPUT-014: Replay takes Modern Controls on/off from the live profile

- Severity: medium (replay determinism; a Modern recording can go off course)
- Status: verified in tests; native replay verification pending
- Reported and last updated (UTC): 2026-10-23
- Affected versions, execution profile, platform and settings: v1.4.0 and
  earlier Windows builds; any replay of a v5-v8 recording made with Modern
  Controls
- Related report, feature notes and source locations:
  [PC controls](../../features/pc-controls.md),
  `src/boot/native_boot.cpp` (`reload_controller_ports`, the replay branch of
  `sample_live_controller`), `include/jfg/runtime/input_replay.hpp`
  (`ReplayModernControls`)

## Symptoms and reproduction

Found by reading the code while planning camera-relative running; not
reproduced in a native run. During replay, `reload_controller_ports` still
hot-reloads the controller profiles and sets `modern_controls_active` from any
player's live `enabled` flag. The replayed sample is copied into the Modern
frame only when that flag is set, and the Modern hooks also check the live
`modern_controls[0].enabled`. A missing Modern profile clears `enabled`, and
without `JFG_CONTROLLER_CONFIG` the reload returns early and the flag stays
false.

So the same Modern recording replays with or without the Modern adapter
depending on the replaying machine's profile and launch environment:

- Launcher replays (which always pass `JFG_CONTROLLER_CONFIG`) use the
  replaying user's style for player 1.
- Headless evidence replays (`scripts/phase95_native_replay.py`,
  `scripts/replay_phase9_capture.ps1`) pass no profile, so Modern recordings
  most likely replay without the Modern adapter.

## Evidence and hypotheses

Every sample recorded while player 1 has Modern selected carries
`kModernActionsFlag` (0x80), including input-blocked samples. Controller
Mapping samples carry flags 0. v1-v4 recordings have no flags field. While a
Modern device waits for release after a device change, samples have flags 0
with zeroed buttons and stick. No format change is needed to recover the style.

## Attempts and results

| Date | Hypothesis and change/experiment | Result | Decision and evidence |
|---|---|---|---|
| 2026-10-23 | Derive player 1's style during replay from the samples as a held state: starts off, 0x80 turns it on, Controller Mapping input (buttons, stick, look, legacy aim modes 1/2) turns it off, neutral flag-0 and disconnected samples keep it | Unit tests pass (`jfg_input_replay_tests`) | Candidate in source; native replays pending |

## Actual resolution

Candidate, not yet verified natively. `jfg::ReplayModernControls` holds the
rule. While a replay is loaded, the native runtime:

- clears the boot-time profile style when the replay loads,
- updates the held state from each replayed sample and applies it to both
  `modern_controls_active` and `modern_controls[0].enabled` (players 2-4 off),
  with the same resets the live path applies on a style change, and
- keeps the live profile reload from changing `enabled` (other profile values
  still reload).

Recording without a replay is unchanged.

## Verification and remaining limits

- Unit tests: held rule starts off, turns on at 0x80, holds across neutral
  flag-0 and disconnected samples, turns off at each kind of Controller Mapping
  input, follows mid-recording switches both ways, and v1-v4 recordings stay
  off.
- Not run: native replays. This needs a private build with the generated root,
  the private recordings and the guarded supervisor. Controller Mapping and
  v1-v4 replays are expected to stay byte-identical. Modern evidence runs (the
  Goldwood bridge `modern_replay` route and the runs sharing its hash triplet)
  are expected to change and need new, developer-approved baselines.
- A gameplay trace event `replay-modern-controls` marks each held-state change.

## Lessons for future ports and changes

Replay must not read machine-local settings that change gameplay. Anything the
replay needs should come from the recording.
