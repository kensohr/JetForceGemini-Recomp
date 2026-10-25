# Fast development builds

`scripts/dev_build.py` builds the game from your ROM like
[`build_from_rom.py`](rom-bootstrap.md), but for day-to-day work. It keeps
one workspace per checkout and rebuilds it in place, so after the first run a
source change rebuilds in seconds instead of a full build.

This script exists only on this fork. `build_from_rom.py` and release builds
are unchanged. For a build to keep or share, see
[Which build to use](#which-build-to-use).

## Setup

- The prerequisites of [Build the game from your ROM](rom-bootstrap.md):
  Python 3.11 or newer, WSL2 with Ubuntu 24.04 and its packages, and Visual
  Studio 2022 (or its Build Tools) with C++ and CMake. The script finds
  Visual Studio's CMake, Ninja and `vcvars64.bat` through `vswhere`, so
  nothing needs to be on `PATH`.
- Optional, but it makes new checkouts and branch switches much faster:
  [sccache](https://github.com/mozilla/sccache).

  ```powershell
  winget install Mozilla.sccache
  ```

  Open a new terminal afterwards so `sccache` is on `PATH`. Without it the
  build prints a warning and compiles directly.

## Everyday use

From PowerShell at the repository root:

```powershell
python scripts/dev_build.py --rom 'D:\Games\my-copy.z64'
```

Run the same command after every change. Add `--play` to start the game after
a successful build. In a git worktree, pass the ROM as an absolute path.

The first run of a checkout takes about as long as `build_from_rom.py`. After
that, a run with no changes takes a few seconds, and a change to one source
file rebuilds in seconds.

## What each run does

Every run works in the same folder,
`%LOCALAPPDATA%\JFG\b\<checkout-hash>\dev` (printed as
`Development workspace:`), and runs only the steps that are out of date. It
prints why each step runs or is skipped:

| Step | Runs when |
| --- | --- |
| dependencies | No [dependency set](#dependencies-are-shared) matches the pinned revisions yet: it is cloned once. Otherwise the set is reused and only checked. |
| generate | No [stored generated code](#generated-code-is-shared) matches the ROM and the generator inputs. |
| configure | There is no CMake cache yet, the last configure failed, the configure arguments changed (including turning sccache on or off), the generated code changed, the build tree was configured by another CMake generator (it is then recreated), or `--clean` or `--reconfigure` is given. Each reason is printed as `Reason: ...`. |
| build | Always; Ninja does nothing when nothing changed. |

The last line lists the steps that ran and were skipped, the total time and
whether sccache was used. Each step's output goes to `dev\logs\<step>.log`,
and a failing step names its log.

The build uses Ninja and `cmake/dev_build.cmake`, which embeds the debug
information in each object file (`/Z7`) and still links a PDB next to the
exe. Otherwise it compiles with the same flags as `build_from_rom.py`, so it
plays at the same speed.

## Options

| Option | Effect |
| --- | --- |
| `--play` | Start the game after a successful build, as `build_from_rom.py --play` does. |
| `--jobs N` | Parallel compile jobs (default: one per logical core). Code generation always uses `build_from_rom.py`'s default. |
| `--reconfigure` | Configure even when nothing requires it. |
| `--clean` | Delete the development workspace and start over. Stored generated code and dependencies are kept. |
| `--regenerate` | Generate the code again, replacing the stored entry. |
| `--no-sccache` | Compile without sccache even when it is installed. |
| `--bundle` | Refresh the [development bundle](#test-in-the-launcher) after the build. |

`python scripts/dev_build.py --help` lists them all, including `--distro`,
`--generation-timeout`, `--bundle-dir`, `--launcher` and `--vc-runtime`.

## Generated code is shared

Generated code is stored once per ROM and set of generator inputs in
`%LOCALAPPDATA%\JFG\g\<key>\` (about 95 MB each) and shared by every checkout
and worktree. Switching branches generates code only when the generator
inputs differ, and switching back reuses the earlier entry.

Old entries are not removed automatically. Delete `g\<key>` folders that no
build uses any more; `dev\dev-build.json` names the `store_key` each
workspace uses.

## Dependencies are shared

The four pinned dependencies (the decompilation, N64Recomp, N64ModernRuntime
and RT64) are cloned once per set of pinned revisions into
`%LOCALAPPDATA%\JFG\d\<key>\`, a dependency set, and shared by every
checkout and worktree. The key comes from their id, URL and commit in
`dependencies.lock.json`, so a new worktree on the same pins clones nothing.
Changing a pin clones a new set; switching back reuses the earlier one.
Each run checks every dependency's revision; a set that fails the check is
moved aside and cloned again. `build_from_rom.py` still uses the checkout's
own `JFG\b\<checkout-hash>\d`.

Old sets are not removed automatically. Delete `d\<key>` folders that no
checkout's pins use any more.

## sccache

When `sccache` is on `PATH`, every compile goes through it. Its cache is
`%LOCALAPPDATA%\JFG\cache\sccache`, capped at 20 GB. After each build a line
reports the hit rate. A new worktree, or a clean rebuild of the same
checkout, compiles almost entirely from the cache. sccache can't speed up
shader compiles, linking, configure or the clone of a new dependency set.

- **Port reserved by Windows.** Hyper-V and WSL reserve TCP port ranges, which
  can change after a reboot (`netsh int ipv4 show excludedportrange
  protocol=tcp`), and sccache's default port 4226 is often inside one. The
  script picks a free port outside those ranges and prints it, e.g.
  `sccache: server port 4400`. Set `SCCACHE_SERVER_PORT` yourself to choose
  one. If the server still can't start, the run stops before configure; build
  with `--no-sccache` or choose another port.
- **Server with other settings.** One sccache server serves every checkout and
  keeps the settings it started with. If the build warns that another server
  is running with other settings, run `sccache --stop-server` while nothing is
  building, then build again. To query the server by hand, set the same
  `SCCACHE_SERVER_PORT` first.

## Test in the launcher

`--play` starts the game without the launcher's settings, so Modern Controls
stay off. To test with the launcher, build the launcher once (again after
launcher changes), then add `--bundle`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_launcher.ps1 -OutputDirectory $env:LOCALAPPDATA\JFG\l
python scripts/dev_build.py --rom 'D:\Games\my-copy.z64' --bundle
```

This refreshes one unpacked bundle, `%LOCALAPPDATA%\JFG\p\dev\`, in place.
It copies only the files that changed and prints the launcher to start. The
bundle shares saves and settings with your other builds.

- Build the launcher and the runtime from the same branch. A launcher from a
  branch with newer settings can write settings the runtime rejects; the
  launcher then reports only "Game exited unexpectedly".
- Close the launcher and the game before a refresh. Otherwise the refresh
  retries briefly, then fails naming the locked file.
- The bundle contains code compiled from your ROM. Keep it local and never
  publish it.

## Debugging in VS Code

The exe is debugged with its PDB; there is no Visual Studio solution. Both
are in `%LOCALAPPDATA%\JFG\b\<checkout-hash>\dev\n\`. With the C/C++
extension, add this to your own `.vscode/launch.json` (don't commit it), with
your checkout hash and ROM path:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "jfg-native-boot (development build)",
      "type": "cppvsdbg",
      "request": "launch",
      "program": "${env:LOCALAPPDATA}\\JFG\\b\\<checkout-hash>\\dev\\n\\jfg-native-boot.exe",
      "args": [
        "--rom", "D:\\Games\\my-copy.z64",
        "--save", "${env:LOCALAPPDATA}\\JFGRecomp\\profiles\\default\\jfg.flash",
        "--controller-pak", "${env:LOCALAPPDATA}\\JFGRecomp\\profiles\\default\\controller-1.pak",
        "--play"
      ],
      "cwd": "${env:LOCALAPPDATA}\\JFG\\b\\<checkout-hash>\\dev\\n",
      "environment": [],
      "console": "integratedTerminal"
    },
    {
      "name": "Attach to the game process",
      "type": "cppvsdbg",
      "request": "attach",
      "processId": "${command:pickProcess}"
    }
  ]
}
```

The game runs in a child process: the exe you start supervises a second
`jfg-native-boot.exe`, which owns the game window. Start the first
configuration, then run **Attach to the game process** and pick the
`jfg-native-boot.exe` whose command line contains `--_jfg-phase6-token`. To
debug with your launcher settings, start the game from the development
bundle and attach the same way.

The build uses release optimisations (`/O2`), so some local variables are
optimised away and stepping can jump. Stop debugging before rebuilding; the
exe and PDB are locked while the debugger holds them.

## Troubleshooting

- **A build stopped or was interrupted, and later runs fail strangely.** Two
  runs on the same checkout at once can corrupt its build tree. Run once with
  `--clean`; stored generated code and dependencies are kept.
- **Every compile fails with an sccache error.** See
  [sccache](#sccache), or build with `--no-sccache`.
- **"Game exited unexpectedly" in the development bundle.** Check that the
  launcher and the runtime were built from the same branch.

## Which build to use

| Build | Use it for |
| --- | --- |
| `dev_build.py` | Day-to-day work: edit, rebuild, play, debug. |
| `build_from_rom.py` | A build from a clean checkout to keep, package as a versioned launcher bundle, or compare against. |

The development bundle is for testing only; package a versioned bundle from
a `build_from_rom.py` build when you want one to keep.
