# INPUT-015: Replay takes Original movement acceleration from the live profile

- Severity: low (replay determinism; only pc_mode 3 Face the camera movement)
- Status: verified in tests; native replay verification pending
- Reported and last updated (UTC): 2026-10-23
- Affected versions, execution profile, platform and settings: v1.4.0 and
  earlier Windows builds; any replay of a Modern Controls keyboard/mouse
  recording (pc_mode 3) with Ground movement Face the camera
- Related report, feature notes and source locations:
  [INPUT-014](replay-modern-controls-live-profile.md),
  [PC controls](../../features/pc-controls.md),
  `src/boot/native_boot.cpp` (`reload_controller_ports`, the replay branch of
  `sample_live_controller`, the normal-ground branch of the Modern movement
  hook), `include/jfg/runtime/input_replay.hpp` (`ReplayModernControls`)

## Symptoms and reproduction

Found by reading the code while triaging the format v9 Ground movement change;
not reproduced in a native run. The recorder writes `pc_frames[0]` after
mapping, so every Modern profile value applied while mapping is already in the
recording. The one value read after recording in a gameplay hook is
`mouse_original_acceleration`: on normal ground in Face the camera, a pc_mode 3
sample eases the velocity with `pc_original_acceleration` when the option is
on. During replay that option came from the live profile, so the same
recording replayed with or without easing depending on the replaying machine.

Run toward input is not affected: there the native handler owns the velocity
and the option does not apply (developer decision, 2026-10-23).

## Attempts and results

| Date | Hypothesis and change/experiment | Result | Decision and evidence |
|---|---|---|---|
| 2026-10-23 | Record the option per sample as `modern_flags` bit 0x40 in format v9 (with Ground movement's 0x20), hold it in `ReplayModernControls` and apply it to player 1 during replay | Unit tests pass (`jfg.input_replay`, `jfg.recorded_input_timeline`) | Candidate in source; native replays pending |

## Actual resolution

Candidate, not yet verified natively.

- Format v9: `kModernOriginalAccelerationFlag` (0x40) is set on each Modern
  sample (bit 7 set) while player 1 has the option on, on any pc_mode. The
  parser accepts it only in v9, with bit 7, on connected samples.
- Playback: `ReplayModernControls::original_acceleration()` holds the value of
  the latest Modern sample; it starts off. While a replay is loaded, native
  boot applies it to `modern_controls[0]` at load and on every replay poll,
  and the profile hot-reload keeps the replayed value. A gameplay trace event
  `replay-original-acceleration` marks each change.
- Older recordings (v1-v8) never set the bit, so they replay without easing.
  A v8 recording made with the option on therefore replays differently than
  before; such recordings were never reproducible across profiles.

## Verification and remaining limits

- Unit tests: parsing, roundtrip, coexistence with scope, roll and Ground
  movement bits, rejections (no bit 7, disconnected, v5-v8), the held rule,
  the v8 fallback, recorder to v9 file to playback for both Ground movement
  modes with the option on and off, and timeline holding across polls, a
  mid-recording change and roll pulses.
- Not run: native replays (needs a private build with the generated root and
  the guarded supervisor). Pending: record pc_mode 3 Face the camera runs with
  the option on and off, replay each on a profile with the opposite setting,
  and compare RDRAM triplets; confirm existing evidence runs stay
  byte-identical.

## Lessons for future ports and changes

Same as INPUT-014: replay must not read machine-local settings that change
gameplay. Anything the replay needs should come from the recording.
