from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scripts.build_paths import MAX_BINARY_ROOT
from scripts.dev_build import (
    ROOT,
    WorkspaceState,
    dev_workspace,
    generated_code_store_root,
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


class GeneratedCodeStoreRootTests(unittest.TestCase):
    def test_store_is_g_next_to_the_checkout_caches(self) -> None:
        base = Path("/" + "x" * 32 + "/JFG")
        self.assertEqual(generated_code_store_root(base), base.resolve() / "g")

    def test_too_long_store_path_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            generated_code_store_root(Path("/" + "x" * 80 + "/JFG"))


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


KEY = "0123456789abcdef"
CONFIGURE = ["-S", "repo", "-B", "dev/n", "-DJFG_GENERATED_ROOT=g/0123456789abcdef/normalized"]


def built_workspace(**changes: object) -> WorkspaceState:
    """State of a workspace whose last run configured and built successfully."""
    fields: dict[str, object] = {
        "store_key": KEY,
        "configure_arguments": list(CONFIGURE),
        "configure_succeeded": True,
        "has_cmake_cache": True,
    }
    fields.update(changes)
    return WorkspaceState(**fields)


class PlanBuildTests(unittest.TestCase):
    def plan(self, state: WorkspaceState, key: str = KEY, configure: list[str] = CONFIGURE, **options: bool):
        return plan_build(state, key, list(configure), **options)

    def test_first_run_configures_and_builds(self) -> None:
        plan = self.plan(WorkspaceState())
        self.assertTrue(plan.configure)
        self.assertTrue(plan.build)

    def test_no_change_only_builds(self) -> None:
        plan = self.plan(built_workspace())
        self.assertFalse(plan.configure)
        self.assertTrue(plan.build)
        self.assertEqual(plan.reasons, ())

    def test_changed_store_key_reconfigures_and_names_both_keys(self) -> None:
        new_key = "fedcba9876543210"
        configure = ["-S", "repo", "-B", "dev/n", "-DJFG_GENERATED_ROOT=g/fedcba9876543210/normalized"]
        plan = self.plan(built_workspace(), key=new_key, configure=configure)
        self.assertTrue(plan.configure)
        self.assertIn("generated-code store key changed: 0123456789abcdef -> fedcba9876543210", plan.reasons)

    def test_regenerated_entry_reconfigures(self) -> None:
        plan = self.plan(built_workspace(), regenerated=True)
        self.assertTrue(plan.configure)
        self.assertIn("configure follows a regeneration", plan.reasons)

    def test_changed_configure_arguments_reconfigure(self) -> None:
        recorded = ["-S", "repo", "-B", "dev/n", "-DJFG_MSVC_GENERATED_COMPILE_JOBS=4"]
        plan = self.plan(built_workspace(configure_arguments=recorded))
        self.assertTrue(plan.configure)
        self.assertEqual(plan.reasons, ("configure arguments changed",))

    def test_failed_or_missing_configure_reconfigures(self) -> None:
        for state in (built_workspace(configure_succeeded=False), built_workspace(has_cmake_cache=False)):
            with self.subTest(state=state):
                self.assertTrue(self.plan(state).configure)

    def test_reconfigure_option_forces_configure(self) -> None:
        plan = self.plan(built_workspace(), reconfigure=True)
        self.assertTrue(plan.configure)
        self.assertIn("--reconfigure given", plan.reasons)

    def test_clean_option_configures_whatever_the_state(self) -> None:
        plan = self.plan(built_workspace(), clean=True)
        self.assertTrue(plan.configure)
        self.assertIn("--clean given", plan.reasons)


if __name__ == "__main__":
    unittest.main()
