"""dev_build.py repeats three steps of build_from_rom.py; these tests catch drift between them.

build_from_rom.main() runs as a black box with fakes for the ROM check, the
toolchain and every subprocess. The commands it would run are compared with
what dev_build.py builds for the same paths, so a change to build_from_rom.py
(for example from upstream) fails here instead of going unnoticed.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts.dev_build import (
    GeneratedCode,
    GenerateSettings,
    configure_arguments,
    play,
    run_generator,
)
from scripts.sccache_launcher import LAUNCHER_VARIABLES

# Imported after scripts.dev_build, which puts scripts/ on sys.path the way build_from_rom.py expects.
import build_from_rom  # noqa: E402

FAKE_CMAKE = "C:/fake/cmake.exe"
DEFAULT_DISTRO = "Ubuntu-24.04"
DEFAULT_GENERATION_TIMEOUT_SECONDS = 3600
FROM_ROM_EXECUTABLE = Path("Release/jfg-native-boot.exe")

# Definitions only build_from_rom.py passes.
FROM_ROM_ONLY_DEFINITIONS = {
    # cmake/dev_build.cmake switches it off for /Z7 and links with /DEBUG itself.
    "JFG_SUPPORT_SYMBOLS",
    # MSBuild-only; Ninja parallelises across files.
    "JFG_MSVC_GENERATED_COMPILE_JOBS",
}
# Definitions only dev_build.py passes.
DEV_ONLY_DEFINITIONS = {
    # Ninja is single-config; the Visual Studio generator picks Release at build time instead.
    "CMAKE_BUILD_TYPE",
    # Ninja from the Visual Studio Build Tools; MSBuild needs none.
    "CMAKE_MAKE_PROGRAM",
    # The sccache compiler launcher is development-build only.
    *LAUNCHER_VARIABLES,
}


def definitions(arguments: list[str]) -> dict[str, str]:
    """Return the -DNAME=VALUE arguments of a configure command as {NAME: VALUE}."""
    found = {}
    for argument in arguments:
        if argument.startswith("-D"):
            name, _, value = argument[2:].partition("=")
            found[name] = value
    return found


def argument_after(arguments: list[str], flag: str) -> str:
    return arguments[arguments.index(flag) + 1]


class FakeProcess:
    """A subprocess.Popen stand-in that records the command and exits 0 with no output."""

    def __init__(self, recorded: list[list[str]], command: list[str], **options: object) -> None:
        recorded.append(list(command))
        self.stdout = iter(())
        if command[1:2] == ["--build"]:
            # The real compile leaves the exe behind; main() checks for it before --play.
            executable = Path(command[2]) / FROM_ROM_EXECUTABLE
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.touch()

    def __enter__(self) -> "FakeProcess":
        return self

    def __exit__(self, *exception: object) -> None:
        return None

    def wait(self) -> int:
        return 0


class FromRomRun:
    """The commands one faked `build_from_rom.py --play` run would have executed."""

    def __init__(self, temp: Path, environment: dict[str, str]) -> None:
        self.rom = temp / "game.z64"
        self.rom.write_bytes(b"")
        self.dependencies = (temp / "deps").resolve()
        cache = (temp / "b").resolve()
        self.commands: list[list[str]] = []
        self.play_call = mock.Mock(return_value=0)
        argv = ["build_from_rom.py", "--rom", str(self.rom), "--dependency-root", str(self.dependencies),
                "--play"]
        with contextlib.ExitStack() as stack:
            for patch in (
                mock.patch.object(sys, "argv", argv),
                mock.patch.dict(os.environ, environment, clear=True),
                mock.patch.object(build_from_rom, "validate_rom", return_value=build_from_rom.ROM_SHA1),
                mock.patch.object(build_from_rom, "find_cmake", return_value=FAKE_CMAKE),
                mock.patch.object(build_from_rom.shutil, "which", side_effect=lambda tool: "C:/fake/" + tool),
                mock.patch.object(build_from_rom, "write_identity"),
                # The temp folder is deeper than the short MSBuild path limit allows; nothing is compiled here.
                mock.patch.object(build_from_rom, "windows_cache", return_value=cache),
                mock.patch.object(build_from_rom, "check_binary_path", side_effect=Path.resolve),
                mock.patch.object(build_from_rom.subprocess, "Popen",
                                  side_effect=lambda command, **options: FakeProcess(self.commands, command)),
                mock.patch.object(build_from_rom.subprocess, "check_output", side_effect=self.pinned_head),
                mock.patch.object(build_from_rom.subprocess, "call", self.play_call),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                stack.enter_context(patch)
            self.exit_code = build_from_rom.main()
        self.workspace = next(cache.glob("w/*"))

    def pinned_head(self, command: list[str], **options: object) -> str:
        """Answer `git -C <dependency> rev-parse HEAD` with the lock file's pin, so main() accepts it."""
        dependency = Path(argument_after(command, "-C"))
        lock = json.loads((build_from_rom.ROOT / "dependencies.lock.json").read_text())
        for entry in lock["repositories"]:
            if self.dependencies / build_from_rom.NAMES.get(entry["id"], "") == dependency:
                return entry["commit"] + "\n"
        raise AssertionError(f"unexpected dependency queried: {dependency}")

    def step(self, first_arguments: list[str]) -> list[str]:
        return next(command for command in self.commands if command[:len(first_arguments)] == first_arguments)

    @property
    def configure(self) -> list[str]:
        return self.step([FAKE_CMAKE, "-S"])

    @property
    def generate(self) -> list[str]:
        return self.step(["wsl"])

    @property
    def executable(self) -> Path:
        return Path(argument_after(self.configure, "-B")) / FROM_ROM_EXECUTABLE


class RecordingRunner:
    """A dev_build.StepRunner stand-in that records the commands instead of running them."""

    def __init__(self) -> None:
        self.commands: list[list[str]] = []

    def run(self, name: str, command: list[str], **options: object) -> None:
        self.commands.append(command)


@unittest.skipUnless(os.name == "nt", "build_from_rom.py runs on Windows only")
class DevBuildParityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = Path(tempfile.mkdtemp(prefix="jfg-parity-"))
        self.addCleanup(shutil.rmtree, self.temp, ignore_errors=True)
        self.local_app_data = self.temp / "local"
        self.environment = {"LOCALAPPDATA": str(self.local_app_data), "JFG_UNRELATED": "dropped",
                            "PATH": os.environ.get("PATH", "")}

    def from_rom_run(self, extra_environment: dict[str, str] | None = None) -> FromRomRun:
        run = FromRomRun(self.temp, {**self.environment, **(extra_environment or {})})
        self.assertEqual(run.exit_code, 0)
        return run

    def test_configure_definitions_match_except_documented_ones(self) -> None:
        run = self.from_rom_run()
        generated = GeneratedCode(normalized=run.workspace / "normalized", libultra=run.workspace / "libultra.json",
                                  audio=run.workspace / "audio")
        dev = configure_arguments(Path(argument_after(run.configure, "-B")), generated, run.dependencies,
                                  Path("C:/fake/ninja.exe"), checkout=build_from_rom.ROOT)

        from_rom_definitions = definitions(run.configure)
        dev_definitions = definitions(dev)
        for name in FROM_ROM_ONLY_DEFINITIONS:
            self.assertIn(name, from_rom_definitions, "stale exception: build_from_rom.py no longer passes it")
            del from_rom_definitions[name]
        for name in DEV_ONLY_DEFINITIONS:
            self.assertIn(name, dev_definitions, "stale exception: dev_build.py no longer passes it")
            del dev_definitions[name]
        self.assertEqual(dev_definitions, from_rom_definitions)
        self.assertEqual(argument_after(dev, "-S"), argument_after(run.configure, "-S"))

    def test_generate_command_matches(self) -> None:
        run = self.from_rom_run()
        runner = RecordingRunner()
        settings = GenerateSettings(rom=run.rom, dependencies=run.dependencies, distro=DEFAULT_DISTRO,
                                    timeout_seconds=DEFAULT_GENERATION_TIMEOUT_SECONDS, checkout=build_from_rom.ROOT)
        run_generator(runner, run.workspace, settings)
        # Same workspace and default --jobs: dev_build.py's GENERATION_JOBS is build_from_rom.py's default.
        self.assertEqual(runner.commands, [run.generate])

    def assert_play_matches(self, extra_environment: dict[str, str]) -> None:
        run = self.from_rom_run(extra_environment)
        dev_call = mock.Mock(return_value=0)
        with mock.patch.dict(os.environ, {**self.environment, **extra_environment}, clear=True), \
                mock.patch("scripts.dev_build.subprocess.call", dev_call):
            play(run.executable, run.rom)
        self.assertEqual(dev_call.call_args, run.play_call.call_args)
        self.assertFalse(any(key.startswith("JFG_") for key in dev_call.call_args.kwargs["env"]))

    def test_play_matches_with_the_default_data_root(self) -> None:
        self.assert_play_matches({})

    def test_play_matches_with_a_data_root_override(self) -> None:
        self.assert_play_matches({"JFG_DATA_ROOT": str(self.temp / "data")})


if __name__ == "__main__":
    unittest.main()
