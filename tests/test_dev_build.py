from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scripts.build_paths import MAX_BINARY_ROOT
from scripts.dev_build import (
    ROOT,
    WorkspaceState,
    changed_generator_inputs,
    dev_workspace,
    generator_input_fingerprint,
    generator_input_paths,
    native_build_tree,
    plan_build,
)

SHA256_OF_ABC = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
SHA256_OF_EMPTY = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"


class DevWorkspaceTests(unittest.TestCase):
    # Longer than a typical %LOCALAPPDATA%\JFG\b (about 34 characters); nothing is created there.
    CACHE = Path("/" + "x" * 32 + "/JFG/b")

    def setUp(self) -> None:
        self.checkout = Path("/work/JetForceGemini-Recomp")
        self.other_checkout = Path("/work/JetForceGemini-Recomp/.claude/worktrees/agent-1")

    def test_same_checkout_gets_the_same_workspace(self) -> None:
        first = dev_workspace(self.checkout, self.CACHE)
        second = dev_workspace(self.checkout, self.CACHE)
        self.assertEqual(first, second)
        self.assertEqual(first.name, "dev")

    def test_other_checkout_gets_another_workspace(self) -> None:
        mine = dev_workspace(self.checkout, self.CACHE)
        theirs = dev_workspace(self.other_checkout, self.CACHE)
        self.assertNotEqual(mine, theirs)

    def test_build_tree_stays_within_the_msbuild_path_limit(self) -> None:
        build = native_build_tree(dev_workspace(self.checkout, self.CACHE))
        self.assertLessEqual(len(str(build)), MAX_BINARY_ROOT)


class GeneratorInputFingerprintTests(unittest.TestCase):
    def test_names_files_by_repo_relative_posix_path_and_hashes_their_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "scripts").mkdir()
            (root / "scripts/generate.py").write_bytes(b"abc")
            (root / "empty.json").write_bytes(b"")
            fingerprint = generator_input_fingerprint(root, ["scripts/generate.py", "empty.json"])
        self.assertEqual(fingerprint, {"empty.json": SHA256_OF_EMPTY, "scripts/generate.py": SHA256_OF_ABC})
        self.assertEqual(list(fingerprint), ["empty.json", "scripts/generate.py"])

    def test_missing_input_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                generator_input_fingerprint(Path(directory), ["scripts/missing.py"])

    def test_inputs_cover_generator_scripts_patches_copied_sources_and_lock(self) -> None:
        paths = generator_input_paths(ROOT)
        for expected in ("scripts/generate_from_rom.py", "scripts/build_private_generated_root.py",
                         "scripts/original_timing_generation.py", "scripts/transform_private_n64recomp_inputs.py",
                         "patches/n64recomp/series.json", "patches/n64recomp/series/0001-build-regression-target.patch",
                         "src/bootstrap/brokered-rsp.hpp", "include/jfg/boot/original_timing_fast.h",
                         "dependencies.lock.json"):
            self.assertIn(expected, paths)
        self.assertEqual(paths, sorted(set(paths)))
        self.assertTrue(all((ROOT / path).is_file() for path in paths))

    def test_changed_inputs_name_edited_added_and_removed_files(self) -> None:
        recorded = {"a.py": "1", "b.py": "2", "gone.py": "3"}
        current = {"a.py": "1", "b.py": "changed", "new.py": "4"}
        self.assertEqual(changed_generator_inputs(recorded, current), ["b.py", "gone.py", "new.py"])


ROM_SHA1 = "493ced9008dbe932d6e91179b68e8630cf23a023"
GENERATED_FILES = ("normalized/sources.json", "libultra.json", "audio/production-audio-adapter.cpp",
                   "audio/brokered-rsp.hpp", "audio/brokered-jfg-audio-probe.cpp", "audio/jfg-audio-probe.cpp",
                   "normalized/jfg_original_timing.h", "normalized/original-timing.json",
                   "normalized/jfg_original_timing_fast.h", "normalized/funcs.h")
INPUTS = {"scripts/build_private_generated_root.py": "aa", "scripts/generate_from_rom.py": "bb"}
CONFIGURE = ["-S", "repo", "-B", "dev/n", "-DJFG_MSVC_GENERATED_COMPILE_JOBS=8"]


def built_workspace(**changes: object) -> WorkspaceState:
    """State of a workspace whose last run generated, configured and built successfully."""
    digests = {name: "digest-" + str(index) for index, name in enumerate(GENERATED_FILES)}
    fields: dict[str, object] = {
        "generation": {"complete": True, "rom_sha1": ROM_SHA1, "files": dict(digests)},
        "generated_file_digests": dict(digests),
        "generator_inputs": dict(INPUTS),
        "configure_arguments": list(CONFIGURE),
        "configure_succeeded": True,
        "has_cmake_cache": True,
    }
    fields.update(changes)
    return WorkspaceState(**fields)


class PlanBuildTests(unittest.TestCase):
    def plan(self, state: WorkspaceState, **options: bool):
        return plan_build(state, dict(INPUTS), list(CONFIGURE), **options)

    def test_first_run_generates_configures_and_builds(self) -> None:
        plan = self.plan(WorkspaceState())
        self.assertTrue(plan.generate)
        self.assertTrue(plan.configure)
        self.assertTrue(plan.build)

    def test_no_change_only_builds(self) -> None:
        plan = self.plan(built_workspace())
        self.assertFalse(plan.generate)
        self.assertFalse(plan.configure)
        self.assertTrue(plan.build)

    def test_invalid_generation_regenerates_and_configures(self) -> None:
        good = built_workspace()
        tampered = dict(good.generated_file_digests, **{"normalized/funcs.h": "other"})
        missing = dict(good.generated_file_digests, **{"libultra.json": None})
        cases = {
            "incomplete": built_workspace(generation=dict(good.generation, complete=False)),
            "other ROM": built_workspace(generation=dict(good.generation, rom_sha1="0" * 40)),
            "file list": built_workspace(generation=dict(good.generation, files={"libultra.json": "x"})),
            "changed output": built_workspace(generated_file_digests=tampered),
            "missing output": built_workspace(generated_file_digests=missing),
        }
        for name, state in cases.items():
            with self.subTest(name):
                plan = self.plan(state)
                self.assertTrue(plan.generate)
                self.assertTrue(plan.configure)

    def test_changed_generator_input_regenerates_and_names_the_file(self) -> None:
        recorded = dict(INPUTS, **{"scripts/build_private_generated_root.py": "old"})
        plan = self.plan(built_workspace(generator_inputs=recorded))
        self.assertTrue(plan.generate)
        self.assertTrue(plan.configure)
        self.assertIn("generator inputs changed: build_private_generated_root.py", plan.reasons)

    def test_changed_configure_arguments_reconfigure_without_generating(self) -> None:
        recorded = ["-S", "repo", "-B", "dev/n", "-DJFG_MSVC_GENERATED_COMPILE_JOBS=4"]
        plan = self.plan(built_workspace(configure_arguments=recorded))
        self.assertFalse(plan.generate)
        self.assertTrue(plan.configure)

    def test_failed_or_missing_configure_reconfigures(self) -> None:
        for state in (built_workspace(configure_succeeded=False), built_workspace(has_cmake_cache=False)):
            with self.subTest(state=state):
                plan = self.plan(state)
                self.assertFalse(plan.generate)
                self.assertTrue(plan.configure)

    def test_reconfigure_option_forces_configure_only(self) -> None:
        plan = self.plan(built_workspace(), reconfigure=True)
        self.assertFalse(plan.generate)
        self.assertTrue(plan.configure)
        self.assertIn("--reconfigure given", plan.reasons)

    def test_clean_option_starts_over_whatever_the_state(self) -> None:
        plan = self.plan(built_workspace(), clean=True)
        self.assertTrue(plan.generate)
        self.assertTrue(plan.configure)
        self.assertIn("--clean given", plan.reasons)


if __name__ == "__main__":
    unittest.main()
