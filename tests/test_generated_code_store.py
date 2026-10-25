from __future__ import annotations

import hashlib
import json
import os
import tempfile
import time
import unittest
from pathlib import Path

from scripts.dev_build import generator_input_fingerprint
from scripts.generated_code_store import (
    StoreOutcome,
    ensure_store_entry,
    entry_problem,
    finish_staged_entry,
    remove_stale_leftovers,
    store_entry_path,
    store_key,
)

ROM_SHA1 = "493ced9008dbe932d6e91179b68e8630cf23a023"


def write_checkout(root: Path, generator_text: bytes = b"generate") -> None:
    (root / "scripts").mkdir(parents=True)
    (root / "scripts/generate_from_rom.py").write_bytes(generator_text)
    (root / "dependencies.lock.json").write_bytes(b"{}")


INPUT_PATHS = ["scripts/generate_from_rom.py", "dependencies.lock.json"]


class StoreKeyTests(unittest.TestCase):
    def fingerprint_of_checkout(self, checkout_name: str, generator_text: bytes = b"generate") -> dict[str, str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / checkout_name
            write_checkout(root, generator_text)
            return generator_input_fingerprint(root, INPUT_PATHS)

    def test_same_inputs_in_two_checkout_paths_give_the_same_key(self) -> None:
        main = self.fingerprint_of_checkout("JetForceGemini-Recomp")
        worktree = self.fingerprint_of_checkout("wt-04")
        self.assertEqual(store_key(ROM_SHA1, main), store_key(ROM_SHA1, worktree))

    def test_key_is_16_lowercase_hex_characters(self) -> None:
        key = store_key(ROM_SHA1, {"scripts/generate_from_rom.py": "aa"})
        self.assertRegex(key, r"^[0-9a-f]{16}$")

    def test_known_inputs_give_a_known_key(self) -> None:
        # sha256('{"generator_inputs":{"a.py":"aa"},"rom_sha1":"r"}')[:16], computed independently.
        self.assertEqual(store_key("r", {"a.py": "aa"}), "584634e8d942a49a")

    def test_changed_digest_changes_the_key(self) -> None:
        original = self.fingerprint_of_checkout("checkout")
        edited = self.fingerprint_of_checkout("checkout", b"generate differently")
        self.assertNotEqual(store_key(ROM_SHA1, original), store_key(ROM_SHA1, edited))

    def test_changed_rom_changes_the_key(self) -> None:
        fingerprint = {"scripts/generate_from_rom.py": "aa"}
        self.assertNotEqual(store_key(ROM_SHA1, fingerprint), store_key("0" * 40, fingerprint))

    def test_added_input_changes_the_key(self) -> None:
        fingerprint = {"scripts/generate_from_rom.py": "aa"}
        added = dict(fingerprint, **{"patches/n64recomp/new.patch": "bb"})
        self.assertNotEqual(store_key(ROM_SHA1, fingerprint), store_key(ROM_SHA1, added))


KEY = "0123456789abcdef"
MARKER = "complete.txt"


class FakeGenerator:
    """Stands in for generate_from_rom.py: writes a marker file into the staging folder."""

    def __init__(self, content: str = "generated", error: Exception | None = None) -> None:
        self.content = content
        self.error = error
        self.staging_folders: list[Path] = []

    def __call__(self, staging: Path) -> None:
        self.staging_folders.append(staging)
        (staging / "partial.txt").write_text("half-written")
        if self.error:
            raise self.error
        (staging / MARKER).write_text(self.content)


def marker_problem(entry: Path) -> str | None:
    """Fake validation: an entry is valid when its marker file says 'generated'."""
    marker = entry / MARKER
    if not marker.is_file():
        return "no marker"
    if marker.read_text() != "generated":
        return "marker changed"
    return None


class StoreEntryLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.store = Path(directory.name) / "g"

    def ensure(self, generator: FakeGenerator, **options: bool):
        return ensure_store_entry(self.store, KEY, generator, marker_problem, **options)

    def test_entry_path_is_the_key_inside_the_store(self) -> None:
        self.assertEqual(store_entry_path(self.store, KEY), self.store / KEY)

    def test_missing_entry_is_generated_in_a_staging_folder_and_published(self) -> None:
        generator = FakeGenerator()
        result = self.ensure(generator)
        self.assertEqual(result.outcome, StoreOutcome.GENERATED)
        self.assertEqual(result.path, self.store / KEY)
        self.assertEqual((result.path / MARKER).read_text(), "generated")
        [staging] = generator.staging_folders
        self.assertNotEqual(staging, result.path)
        self.assertEqual(staging.parent, self.store)
        self.assertFalse(staging.exists())
        self.assertEqual(sorted(path.name for path in self.store.iterdir()), [KEY])

    def test_valid_entry_is_reused_without_generating(self) -> None:
        self.ensure(FakeGenerator())
        generator = FakeGenerator()
        result = self.ensure(generator)
        self.assertEqual(result.outcome, StoreOutcome.REUSED)
        self.assertEqual(result.path, self.store / KEY)
        self.assertEqual(generator.staging_folders, [])

    def test_failed_generation_publishes_no_entry_and_its_staging_folder_is_never_used(self) -> None:
        with self.assertRaises(RuntimeError):
            self.ensure(FakeGenerator(error=RuntimeError("generator killed")))
        self.assertFalse((self.store / KEY).exists())
        retry = FakeGenerator()
        result = self.ensure(retry)
        self.assertEqual(result.outcome, StoreOutcome.GENERATED)
        self.assertEqual(len(retry.staging_folders), 1)
        self.assertEqual((result.path / MARKER).read_text(), "generated")

    def test_staging_folder_that_fails_validation_is_not_published(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "marker changed"):
            self.ensure(FakeGenerator(content="wrong"))
        self.assertFalse((self.store / KEY).exists())

    def test_invalid_entry_is_moved_aside_and_regenerated(self) -> None:
        self.ensure(FakeGenerator())
        (self.store / KEY / MARKER).write_text("corrupted")
        generator = FakeGenerator()
        with self.assertLogs("generated_code_store", "WARNING") as logs:
            result = self.ensure(generator)
        self.assertEqual(result.outcome, StoreOutcome.REPLACED)
        self.assertEqual(result.reason, "marker changed")
        self.assertEqual(len(generator.staging_folders), 1)
        self.assertEqual((result.path / MARKER).read_text(), "generated")
        [moved_aside] = [path for path in self.store.iterdir() if path.name.startswith(KEY + ".bad-")]
        self.assertEqual((moved_aside / MARKER).read_text(), "corrupted")
        self.assertIn("marker changed", " ".join(logs.output))

    def test_regenerate_replaces_a_valid_entry(self) -> None:
        self.ensure(FakeGenerator())
        generator = FakeGenerator()
        result = self.ensure(generator, regenerate=True)
        self.assertEqual(result.outcome, StoreOutcome.REPLACED)
        self.assertEqual(result.reason, "--regenerate given")
        self.assertEqual(len(generator.staging_folders), 1)
        self.assertEqual(len([path for path in self.store.iterdir() if ".bad-" in path.name]), 1)

    def test_lost_race_discards_the_staging_folder_and_uses_the_existing_entry(self) -> None:
        def generate_while_another_worktree_publishes(staging: Path) -> None:
            FakeGenerator()(staging)
            other = self.store / "other-worktree-staging"
            other.mkdir()
            (other / MARKER).write_text("generated")
            (other / "winner.txt").write_text("other worktree")
            os.rename(other, self.store / KEY)

        result = ensure_store_entry(self.store, KEY, generate_while_another_worktree_publishes, marker_problem)
        self.assertEqual(result.outcome, StoreOutcome.LOST_RACE)
        self.assertEqual(result.path, self.store / KEY)
        self.assertEqual((result.path / "winner.txt").read_text(), "other worktree")
        self.assertEqual(sorted(path.name for path in self.store.iterdir()), [KEY])


class StaleLeftoverTests(unittest.TestCase):
    DAY_SECONDS = 24 * 3600

    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.store = Path(directory.name) / "g"
        self.store.mkdir()
        self.now = time.time()

    def folder(self, name: str, age_seconds: float) -> Path:
        path = self.store / name
        path.mkdir()
        (path / "file.txt").write_text("x")
        stamp = self.now - age_seconds
        os.utime(path, (stamp, stamp))
        return path

    def test_removes_staging_and_moved_aside_folders_older_than_a_day(self) -> None:
        old_staging = self.folder(KEY + ".tmp-0a0b0c0d", self.DAY_SECONDS + 60)
        old_bad = self.folder(KEY + ".bad-0a0b0c0d", 3 * self.DAY_SECONDS)
        with self.assertLogs("generated_code_store", "INFO"):
            removed = remove_stale_leftovers(self.store, now=self.now)
        self.assertEqual(sorted(removed), sorted([old_staging, old_bad]))
        self.assertFalse(old_staging.exists())
        self.assertFalse(old_bad.exists())

    def test_keeps_young_leftovers_and_every_entry(self) -> None:
        young_staging = self.folder(KEY + ".tmp-0a0b0c0d", 3600)
        young_bad = self.folder(KEY + ".bad-0a0b0c0d", 3600)
        old_entry = self.folder(KEY, 30 * self.DAY_SECONDS)
        self.assertEqual(remove_stale_leftovers(self.store, now=self.now), [])
        for path in (young_staging, young_bad, old_entry):
            self.assertTrue(path.is_dir())

    def test_missing_store_has_nothing_to_remove(self) -> None:
        self.assertEqual(remove_stale_leftovers(self.store / "missing", now=self.now), [])


GENERATED_FILES = ("normalized/sources.json", "libultra.json", "audio/production-audio-adapter.cpp",
                   "audio/brokered-rsp.hpp", "audio/brokered-jfg-audio-probe.cpp", "audio/jfg-audio-probe.cpp",
                   "normalized/jfg_original_timing.h", "normalized/original-timing.json",
                   "normalized/jfg_original_timing_fast.h", "normalized/funcs.h")
INPUTS = {"scripts/generate_from_rom.py": "aa"}


def write_generator_output(workspace: Path, rom_sha1: str = ROM_SHA1) -> None:
    """Lay out what generate_from_rom.py leaves in its --workspace, outputs and intermediates."""
    digests = {}
    for name in GENERATED_FILES:
        path = workspace / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("content of " + name)
        digests[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    (workspace / "normalized/funcs_0.c").write_text("void f(void) {}")
    (workspace / "generation.json").write_text(json.dumps({"complete": True, "rom_sha1": rom_sha1,
                                                           "files": digests}))
    for folder in ("transform", "context", "helper-task", "helper-pi-init"):
        (workspace / folder).mkdir()
        (workspace / folder / "intermediate.txt").write_text("x")
    for name in ("layout.json", "linux-workspace.txt"):
        (workspace / name).write_text("x")
    (workspace / "normalize.log").write_text("log")


class StoreEntryContentTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.entry = Path(directory.name) / KEY
        self.entry.mkdir()
        write_generator_output(self.entry)

    def finish(self) -> None:
        finish_staged_entry(self.entry, KEY, ROM_SHA1, INPUTS)

    def test_finishing_keeps_only_what_a_build_needs_and_records_the_entry(self) -> None:
        self.finish()
        names = sorted(path.name for path in self.entry.iterdir())
        self.assertEqual(names, ["audio", "generation.json", "libultra.json", "normalize.log", "normalized",
                                 "store-entry.json"])
        record = json.loads((self.entry / "store-entry.json").read_text())
        self.assertEqual(record["key"], KEY)
        self.assertEqual(record["rom_sha1"], ROM_SHA1)
        self.assertEqual(record["generator_inputs"], INPUTS)
        self.assertIn("created", record)

    def test_finished_entry_is_valid(self) -> None:
        self.finish()
        self.assertIsNone(entry_problem(self.entry, KEY, ROM_SHA1))

    def test_entry_without_its_record_or_for_another_key_is_invalid(self) -> None:
        self.assertEqual(entry_problem(self.entry, KEY, ROM_SHA1), "no store-entry.json")
        self.finish()
        self.assertEqual(entry_problem(self.entry, "f" * 16, ROM_SHA1), "store-entry.json names another key")

    def test_generation_checks_of_resume_native_apply(self) -> None:
        self.finish()
        generation_path = self.entry / "generation.json"
        good = json.loads(generation_path.read_text())
        cases = {
            "generation.json is incomplete": dict(good, complete=False),
            "generated from another ROM": dict(good, rom_sha1="0" * 40),
            "generation.json lists unexpected files": dict(good, files={"libultra.json": "x"}),
        }
        for expected, generation in cases.items():
            with self.subTest(expected):
                generation_path.write_text(json.dumps(generation))
                self.assertEqual(entry_problem(self.entry, KEY, ROM_SHA1), expected)
        generation_path.unlink()
        self.assertEqual(entry_problem(self.entry, KEY, ROM_SHA1), "no generation.json")

    def test_corrupted_or_missing_generated_file_is_named(self) -> None:
        self.finish()
        (self.entry / "normalized/funcs.h").write_text("corrupted")
        (self.entry / "libultra.json").unlink()
        self.assertEqual(entry_problem(self.entry, KEY, ROM_SHA1),
                         "generated files missing or changed: libultra.json, normalized/funcs.h")


if __name__ == "__main__":
    unittest.main()
