from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from scripts.dev_build import (
    GeneratedCode,
    WorkspaceState,
    argument_parser,
    build_command,
    cache_generator,
    configure_arguments,
    parse_environment_listing,
    plan_build,
    runtime_executable,
    visual_studio_tools,
)

BUILD = Path("/cache/dev/n")
DEPENDENCIES = Path("/cache/d")
NINJA = Path("/vs/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe")
GENERATED = GeneratedCode(normalized=Path("/cache/dev/normalized"), libultra=Path("/cache/dev/libultra.json"),
                          audio=Path("/cache/dev/audio"))


def ninja_configure_arguments() -> list[str]:
    return configure_arguments(BUILD, GENERATED, DEPENDENCIES, NINJA)


class NinjaConfigureArgumentTests(unittest.TestCase):
    def test_configures_a_single_config_ninja_release_tree_with_buildtools_ninja(self) -> None:
        arguments = ninja_configure_arguments()
        self.assertEqual(arguments[arguments.index("-G") + 1], "Ninja")
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", arguments)
        self.assertIn("-DCMAKE_MAKE_PROGRAM=" + str(NINJA), arguments)
        self.assertNotIn("-A", arguments)

    def test_loads_the_dev_cache_file(self) -> None:
        arguments = ninja_configure_arguments()
        cache_file = Path(arguments[arguments.index("-C") + 1])
        self.assertEqual(cache_file.as_posix().rsplit("/", 2)[-2:], ["cmake", "dev_build.cmake"])
        self.assertTrue(cache_file.is_file())

    def test_leaves_symbols_and_msbuild_jobs_to_the_cache_file_and_ninja(self) -> None:
        arguments = ninja_configure_arguments()
        for left_out in ("JFG_SUPPORT_SYMBOLS", "JFG_MSVC_GENERATED_COMPILE_JOBS"):
            self.assertFalse([argument for argument in arguments if left_out in argument], left_out)

    def test_keeps_build_from_rom_definitions(self) -> None:
        arguments = ninja_configure_arguments()
        for expected in ("-DBUILD_TESTING=OFF", "-DJFG_ENABLE_GENERATED_CODE=ON", "-DJFG_BUILD_PHASE6_NATIVE_BOOT=ON",
                         "-DJFG_BUILD_PHASE8_LIVE=ON", "-DJFG_ENABLE_RT64=ON",
                         "-DJFG_GENERATED_ROOT=" + str(GENERATED.normalized),
                         "-DJFG_RT64_ROOT=" + str(DEPENDENCIES / "rt64")):
            self.assertIn(expected, arguments)


class RuntimeExecutableTests(unittest.TestCase):
    def test_single_config_ninja_puts_the_exe_in_the_build_tree_root(self) -> None:
        self.assertEqual(runtime_executable(BUILD), BUILD / "jfg-native-boot.exe")


class JobsTests(unittest.TestCase):
    def test_jobs_default_to_the_logical_core_count(self) -> None:
        arguments = argument_parser().parse_args(["--rom", "game.z64"])
        self.assertEqual(arguments.jobs, min(os.cpu_count(), 32))

    def test_jobs_are_a_build_argument_only(self) -> None:
        command = build_command("cmake", BUILD, 12)
        self.assertEqual(command[command.index("--parallel") + 1], "12")
        self.assertIn("jfg-native-boot", command)

    def test_changing_jobs_does_not_reconfigure(self) -> None:
        # Configure arguments don't depend on jobs, so a run with other jobs
        # compares equal to the recorded ones.
        state = configured_workspace()
        plan = plan_build(state, dict(INPUTS), ninja_configure_arguments())
        self.assertFalse(plan.configure)


ROM_SHA1 = "493ced9008dbe932d6e91179b68e8630cf23a023"
GENERATED_FILES = ("normalized/sources.json", "libultra.json", "audio/production-audio-adapter.cpp",
                   "audio/brokered-rsp.hpp", "audio/brokered-jfg-audio-probe.cpp", "audio/jfg-audio-probe.cpp",
                   "normalized/jfg_original_timing.h", "normalized/original-timing.json",
                   "normalized/jfg_original_timing_fast.h", "normalized/funcs.h")
INPUTS = {"scripts/generate_from_rom.py": "bb"}


def configured_workspace(**changes: object) -> WorkspaceState:
    """State of a workspace that a Ninja configure and build finished in."""
    digests = {name: "digest-" + str(index) for index, name in enumerate(GENERATED_FILES)}
    fields: dict[str, object] = {
        "generation": {"complete": True, "rom_sha1": ROM_SHA1, "files": dict(digests)},
        "generated_file_digests": dict(digests),
        "generator_inputs": dict(INPUTS),
        "configure_arguments": ninja_configure_arguments(),
        "configure_succeeded": True,
        "has_cmake_cache": True,
        "cmake_generator": "Ninja",
    }
    fields.update(changes)
    return WorkspaceState(**fields)


class GeneratorMismatchTests(unittest.TestCase):
    def plan(self, state: WorkspaceState):
        return plan_build(state, dict(INPUTS), ninja_configure_arguments())

    def test_ninja_tree_is_kept(self) -> None:
        plan = self.plan(configured_workspace())
        self.assertFalse(plan.configure)
        self.assertFalse(plan.fresh_build_tree)

    def test_visual_studio_tree_is_configured_fresh_without_regenerating(self) -> None:
        plan = self.plan(configured_workspace(cmake_generator="Visual Studio 17 2022"))
        self.assertFalse(plan.generate)
        self.assertTrue(plan.configure)
        self.assertTrue(plan.fresh_build_tree)
        self.assertIn("build tree was configured with Visual Studio 17 2022, not Ninja: starting it fresh",
                      plan.reasons)

    def test_visual_studio_tree_is_also_started_fresh_when_regenerating(self) -> None:
        plan = self.plan(configured_workspace(cmake_generator="Visual Studio 17 2022", generator_inputs={}))
        self.assertTrue(plan.generate)
        self.assertTrue(plan.fresh_build_tree)

    def test_reads_the_generator_from_the_cmake_cache(self) -> None:
        cache = "# This is the CMakeCache file.\nCMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\nX:BOOL=ON\n"
        self.assertEqual(cache_generator(cache), "Visual Studio 17 2022")
        self.assertIsNone(cache_generator("X:BOOL=ON\n"))


class VisualStudioToolsTests(unittest.TestCase):
    VCVARS = "VC/Auxiliary/Build/vcvars64.bat"
    NINJA = "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"

    def install(self, root: Path, *relative_paths: str) -> Path:
        for relative in relative_paths:
            (root / relative).parent.mkdir(parents=True, exist_ok=True)
            (root / relative).write_bytes(b"")
        return root

    def test_finds_vcvars_and_ninja_in_the_install(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            install = self.install(Path(directory), self.VCVARS, self.NINJA)
            tools = visual_studio_tools(install)
        self.assertEqual(tools.vcvars, install / self.VCVARS)
        self.assertEqual(tools.ninja, install / self.NINJA)

    def test_missing_tool_fails_naming_the_file(self) -> None:
        for present, missing in ((self.VCVARS, "ninja.exe"), (self.NINJA, "vcvars64.bat")):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                install = self.install(Path(directory), present)
                with self.assertRaisesRegex(ValueError, missing):
                    visual_studio_tools(install)


class EnvironmentListingTests(unittest.TestCase):
    def test_parses_set_output_after_the_marker(self) -> None:
        listing = ("**********\r\n** Visual Studio 2022 Developer Command Prompt\r\n"
                   "===ENVIRONMENT===\r\nINCLUDE=C:/vc/include;C:/sdk\r\nPath=C:/vc/bin;C:/Windows\r\n"
                   "=C:=C:/work\r\nEMPTY=\r\n")
        environment = parse_environment_listing(listing)
        self.assertEqual(environment["INCLUDE"], "C:/vc/include;C:/sdk")
        self.assertEqual(environment["Path"], "C:/vc/bin;C:/Windows")
        self.assertEqual(environment["EMPTY"], "")
        self.assertEqual(set(environment), {"INCLUDE", "Path", "EMPTY"})

    def test_listing_without_marker_is_an_error(self) -> None:
        with self.assertRaises(ValueError):
            parse_environment_listing("vcvars failed\r\n")


if __name__ == "__main__":
    unittest.main()
