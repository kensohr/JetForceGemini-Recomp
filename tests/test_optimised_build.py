"""ROM-free checks for the optimised from-ROM build (--release-optimizations)."""
import contextlib
import hashlib
import io
import json
import os
import shutil
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import optimised_build
import build_from_rom

GENERATION_FILES = ("normalized/sources.json", "libultra.json", "audio/production-audio-adapter.cpp",
                    "audio/brokered-rsp.hpp", "audio/brokered-jfg-audio-probe.cpp", "audio/jfg-audio-probe.cpp",
                    "normalized/jfg_original_timing.h", "normalized/original-timing.json",
                    "normalized/jfg_original_timing_fast.h", "normalized/funcs.h")


def write_timing_record(workspace: Path, has_native_spans: bool) -> None:
    # Minimal stand-in for normalized/original-timing.json after generation.
    record = {"schema": 1, "instruction_sites": 1, "paired_transfers": 1}
    if has_native_spans:
        record["native_spans"] = {"schema": 1}
    normalized = workspace / "normalized"
    normalized.mkdir(parents=True)
    (normalized / "original-timing.json").write_text(json.dumps(record))


class ResumeModeTests(unittest.TestCase):
    def test_matching_modes_resume(self):
        for has_native_spans in (False, True):
            with self.subTest(optimised=has_native_spans), tempfile.TemporaryDirectory() as directory:
                workspace = Path(directory)
                write_timing_record(workspace, has_native_spans)
                optimised_build.check_resume(workspace, is_enabled=has_native_spans)

    def test_mismatched_modes_ask_for_a_new_build(self):
        for has_native_spans in (False, True):
            with self.subTest(optimised=has_native_spans), tempfile.TemporaryDirectory() as directory:
                workspace = Path(directory)
                write_timing_record(workspace, has_native_spans)
                with self.assertRaisesRegex(ValueError, "start a new build"):
                    optimised_build.check_resume(workspace, is_enabled=not has_native_spans)


class FakeProcess:
    """Stands in for every build step; compile leaves the executable behind."""
    def __init__(self, command, **_):
        self.stdout = iter(())
        if "--build" in command:
            executable = Path(command[command.index("--build") + 1]) / "Release/jfg-native-boot.exe"
            executable.parent.mkdir(parents=True)
            executable.touch()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def wait(self):
        return 0


def pinned_commit(command, **_):
    lock = json.loads((ROOT / "dependencies.lock.json").read_text())
    folder = Path(command[command.index("-C") + 1]).name
    return next(entry["commit"] for entry in lock["repositories"]
                if build_from_rom.NAMES.get(entry["id"]) == folder)


def write_generated_workspace(workspace: Path, has_native_spans: bool) -> None:
    write_timing_record(workspace, has_native_spans)
    for name in GENERATION_FILES:
        path = workspace / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            path.write_text(name)
    (workspace / "generation.json").write_text(json.dumps({
        "complete": True, "rom_sha1": build_from_rom.ROM_SHA1,
        "files": {name: hashlib.sha256((workspace / name).read_bytes()).hexdigest()
                  for name in GENERATION_FILES}}))


@unittest.skipUnless(os.name == "nt", "build_from_rom.py runs on Windows only")
class BuildFromRomTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.cache = Path(directory.name)
        self.commands = []

    def run_build(self, *arguments):
        """Run build_from_rom.main() with every tool and build step faked."""
        def record(command, **options):
            self.commands.append(command)
            return FakeProcess(command, **options)
        argv = ["build_from_rom.py", "--rom", str(self.cache / "game.z64"), "--jobs", "4",
                "--dependency-root", str(self.cache / "d"), *arguments]
        with (
            mock.patch.object(sys, "argv", argv),
            mock.patch.object(build_from_rom, "validate_rom"),
            mock.patch.object(build_from_rom, "find_cmake", return_value="cmake"),
            mock.patch.object(build_from_rom.shutil, "which", return_value="tool"),
            mock.patch.object(build_from_rom, "windows_cache", return_value=self.cache),
            mock.patch.object(build_from_rom, "check_binary_path", side_effect=lambda path: path),
            mock.patch.object(build_from_rom, "write_identity"),
            mock.patch.object(build_from_rom.subprocess, "check_output", side_effect=pinned_commit),
            mock.patch.object(build_from_rom.subprocess, "Popen", side_effect=record),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            return build_from_rom.main()

    def step(self, marker):
        return next(command for command in self.commands if marker in command)

    def generate_command(self):
        return self.step("--workspace")

    def configure_command(self):
        return self.step("-S")

    def test_default_build_is_unoptimised(self):
        self.assertEqual(self.run_build(), 0)
        self.assertEqual(self.generate_command()[-2:], ["--jobs", "4"])
        configure = self.configure_command()
        self.assertNotIn("-C", configure)
        self.assertEqual(Path(configure[configure.index("-B") + 1]).name, "n")

    def test_optimised_build_applies_spans_and_release_cache(self):
        self.assertEqual(self.run_build("--release-optimizations"), 0)
        self.assertEqual(self.generate_command()[-3:], ["--jobs", "4", "--native-spans"])
        configure = self.configure_command()
        self.assertEqual(configure[1:3], ["-C", str(ROOT / "cmake/native_release.cmake")])
        self.assertNotIn(Path(configure[configure.index("-B") + 1]).name, ("n", "native"))

    def generated_workspace(self, has_native_spans):
        workspace = self.cache / "w" / "generated"
        write_generated_workspace(workspace, has_native_spans)
        return workspace

    def test_optimised_resume_skips_generation(self):
        workspace = self.generated_workspace(has_native_spans=True)
        self.assertEqual(self.run_build("--resume-native", str(workspace), "--release-optimizations"), 0)
        self.assertFalse(any("--workspace" in command for command in self.commands))
        self.assertEqual(self.configure_command()[1:3], ["-C", str(ROOT / "cmake/native_release.cmake")])
        self.assertTrue((workspace / "o/Release/jfg-native-boot.exe").is_file())

    def test_mismatched_resume_is_refused_before_configuring(self):
        for has_native_spans in (False, True):
            with self.subTest(optimised=has_native_spans):
                workspace = self.generated_workspace(has_native_spans)
                arguments = ["--resume-native", str(workspace)]
                if not has_native_spans:
                    arguments.append("--release-optimizations")
                with self.assertRaisesRegex(ValueError, "start a new build"):
                    self.run_build(*arguments)
                self.assertEqual(self.commands, [])
                shutil.rmtree(workspace)


if __name__ == "__main__":
    unittest.main()
