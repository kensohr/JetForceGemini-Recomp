from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

# dependency_set imports generated_code_store as a top-level module, so its
# StoreOutcome is another class than scripts.generated_code_store's: compare values.
from scripts.dependency_set import (
    PinnedDependency, dependency_set_key, dependency_sets_root, ensure_dependency_set, git_clone,
)

LOCK = {
    "schema_version": 1,
    "repositories": [
        {"id": "jfg-decomp", "url": "https://example.invalid/jfg.git", "commit": "a" * 40},
        {"id": "n64recomp", "url": "https://example.invalid/recomp.git", "commit": "b" * 40},
        {"id": "n64modernruntime", "url": "https://example.invalid/runtime.git", "commit": "c" * 40},
        {"id": "rt64", "url": "https://example.invalid/rt64.git", "commit": "d" * 40},
        {"id": "bizhawk", "url": "https://example.invalid/bizhawk.git", "commit": "e" * 40},
    ],
    "python_packages": [{"name": "jsonschema", "version": "4.0.0"}],
}


def write_lock(folder: Path, lock: dict) -> Path:
    path = folder / "dependencies.lock.json"
    path.write_text(json.dumps(lock), encoding="utf-8")
    return path


def edited_lock(repository_id: str, **changes: str) -> dict:
    lock = copy.deepcopy(LOCK)
    for entry in lock["repositories"]:
        if entry["id"] == repository_id:
            entry.update(changes)
    return lock


class DependencySetKeyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name)

    def key_of(self, lock: dict) -> str:
        return dependency_set_key(write_lock(self.folder, lock))

    def test_key_is_sixteen_hex_digits(self) -> None:
        self.assertRegex(self.key_of(LOCK), r"^[0-9a-f]{16}$")

    def test_key_ignores_lock_entries_that_are_not_pinned_dependencies(self) -> None:
        other_lock = edited_lock("bizhawk", commit="f" * 40)
        other_lock["python_packages"][0]["version"] = "5.0.0"
        self.assertEqual(self.key_of(LOCK), self.key_of(other_lock))

    def test_key_ignores_other_fields_of_pinned_entries(self) -> None:
        self.assertEqual(self.key_of(LOCK), self.key_of(edited_lock("rt64", license="MIT")))

    def test_key_changes_with_a_pinned_commit(self) -> None:
        self.assertNotEqual(self.key_of(LOCK), self.key_of(edited_lock("rt64", commit="f" * 40)))

    def test_key_changes_with_a_pinned_url(self) -> None:
        self.assertNotEqual(self.key_of(LOCK), self.key_of(edited_lock("n64recomp", url="https://example.invalid/x")))


HEAD_FILE_NAME = "fake-head"
PINNED_FOLDERS = {"Jet-Force-Gemini": "a" * 40, "N64Recomp": "b" * 40, "N64ModernRuntime": "c" * 40, "rt64": "d" * 40}


class FakeGit:
    """Stands in for git: a clone is a folder holding its HEAD commit in a file."""

    def __init__(self) -> None:
        self.cloned: list[Path] = []

    def clone(self, pin: PinnedDependency, destination: Path) -> None:
        destination.mkdir(parents=True)
        (destination / HEAD_FILE_NAME).write_text(pin.commit, encoding="utf-8")
        self.cloned.append(destination)

    @staticmethod
    def read_head(checkout: Path) -> str | None:
        head = checkout / HEAD_FILE_NAME
        return head.read_text(encoding="utf-8") if head.is_file() else None


def write_set(entry: Path) -> None:
    """Lay out a complete set the way another run would have published it."""
    for folder, commit in PINNED_FOLDERS.items():
        (entry / folder).mkdir(parents=True)
        (entry / folder / HEAD_FILE_NAME).write_text(commit, encoding="utf-8")


class EnsureDependencySetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.sets = Path(self.directory.name) / "d"
        self.lock_file = write_lock(Path(self.directory.name), LOCK)
        self.key = dependency_set_key(self.lock_file)
        self.git = FakeGit()

    def ensure(self, git: FakeGit | None = None):
        git = git or self.git
        return ensure_dependency_set(self.sets, self.lock_file, clone=git.clone, read_head=git.read_head)

    def test_missing_set_is_cloned_into_staging_and_published(self) -> None:
        result = self.ensure()
        self.assertEqual(result.outcome.value, "generated")
        self.assertEqual(result.path, self.sets / self.key)
        self.assertEqual(sorted(path.name for path in result.path.iterdir()), sorted(PINNED_FOLDERS))
        self.assertEqual(len(self.git.cloned), 4)
        self.assertTrue(all(self.key + ".tmp-" in str(path) for path in self.git.cloned))
        self.assertEqual([path.name for path in self.sets.iterdir()], [self.key])

    def test_existing_valid_set_is_reused_without_cloning(self) -> None:
        write_set(self.sets / self.key)
        result = self.ensure()
        self.assertEqual(result.outcome.value, "reused")
        self.assertEqual(result.path, self.sets / self.key)
        self.assertEqual(self.git.cloned, [])

    def test_interrupted_creation_leaves_nothing_at_the_final_path(self) -> None:
        class InterruptedGit(FakeGit):
            def clone(self, pin: PinnedDependency, destination: Path) -> None:
                if pin.id == "n64modernruntime":
                    raise KeyboardInterrupt
                super().clone(pin, destination)

        with self.assertRaises(KeyboardInterrupt):
            self.ensure(InterruptedGit())
        self.assertFalse((self.sets / self.key).exists())
        self.assertEqual(self.ensure().outcome.value, "generated")

    def test_set_with_a_wrong_head_is_moved_aside_and_recreated(self) -> None:
        entry = self.sets / self.key
        write_set(entry)
        (entry / "rt64" / HEAD_FILE_NAME).write_text("f" * 40, encoding="utf-8")
        with self.assertLogs("generated_code_store", "WARNING") as logs:
            result = self.ensure()
        self.assertEqual(result.outcome.value, "replaced")
        self.assertIn("rt64", result.reason)
        self.assertIn("rt64", " ".join(logs.output))
        self.assertEqual(self.git.read_head(entry / "rt64"), "d" * 40)
        self.assertEqual(len([path for path in self.sets.iterdir() if ".bad-" in path.name]), 1)

    def test_set_missing_a_dependency_is_recreated(self) -> None:
        entry = self.sets / self.key
        write_set(entry)
        (entry / "N64Recomp" / HEAD_FILE_NAME).unlink()
        (entry / "N64Recomp").rmdir()
        with self.assertLogs("generated_code_store", "WARNING"):
            result = self.ensure()
        self.assertEqual(result.outcome.value, "replaced")
        self.assertIn("N64Recomp is missing", result.reason)

    def test_lost_publish_race_uses_the_winners_set(self) -> None:
        sets, key = self.sets, self.key

        class RacingGit(FakeGit):
            def clone(self, pin: PinnedDependency, destination: Path) -> None:
                super().clone(pin, destination)
                if pin.id == "rt64":
                    write_set(sets / key)

        result = self.ensure(RacingGit())
        self.assertEqual(result.outcome.value, "lost-race")
        self.assertEqual(result.path, sets / key)
        self.assertEqual([path.name for path in sets.iterdir()], [key])

    def test_changed_pin_gives_a_separate_set_and_restoring_it_reuses_the_old_one(self) -> None:
        first = self.ensure().path
        write_lock(Path(self.directory.name), edited_lock("rt64", commit="f" * 40))
        second = self.ensure().path
        self.assertNotEqual(first, second)
        write_lock(Path(self.directory.name), LOCK)
        cloned_before = len(self.git.cloned)
        restored = self.ensure()
        self.assertEqual((restored.path, restored.outcome.value), (first, "reused"))
        self.assertEqual(len(self.git.cloned), cloned_before)


class GitCloneTests(unittest.TestCase):
    def test_clone_pins_the_commit_and_updates_submodules_with_the_build_git_options(self) -> None:
        commands: list[tuple[str, list[str]]] = []
        pin = PinnedDependency("rt64", "https://example.invalid/rt64.git", "d" * 40)
        destination = Path("sets") / "key.tmp-1" / "rt64"
        git_clone(lambda name, command: commands.append((name, command)))(pin, destination)
        self.assertEqual([name for name, _ in commands], ["clone-rt64", "pin-rt64", "submodules-rt64"])
        clone, pin_command, submodules = (command for _, command in commands)
        self.assertEqual(clone, ["git", "clone", "--config", "core.autocrlf=false", "--config", "core.longpaths=true",
                                 "--no-checkout", pin.url, str(destination)])
        self.assertEqual(pin_command, ["git", "-C", str(destination), "checkout", "--detach", pin.commit])
        self.assertEqual(submodules, ["git", "-c", "core.autocrlf=false", "-c", "core.longpaths=true", "-C",
                                      str(destination), "submodule", "update", "--init", "--recursive"])


class DependencySetsRootTests(unittest.TestCase):
    def test_sets_live_in_d_beside_the_generated_code_store(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory) / "JFG"
            self.assertEqual(dependency_sets_root(base), base.resolve() / "d")


if __name__ == "__main__":
    unittest.main()
