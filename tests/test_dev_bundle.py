from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
import unittest
from pathlib import Path

from scripts.dev_bundle import (
    BundleLockedError, BundleSources, FileFacts, bundle_directory_problem, bundle_location, plan_copies,
    refresh_dev_bundle, vc_runtime_folder,
)
from scripts.dev_build import argument_parser
from scripts.package_beta import GAME_FILES, RUNTIME_FILES, VC_RUNTIME_FILES

EXE = FileFacts(size=100, sha256="a" * 64)
NEW_EXE = FileFacts(size=100, sha256="b" * 64)
SDL = FileFacts(size=20, sha256="c" * 64)
LAUNCHER = FileFacts(size=30, sha256="d" * 64)
NEW_LAUNCHER = FileFacts(size=31, sha256="e" * 64)
SOURCES = {"jfg-native-boot.exe": EXE, "SDL2.dll": SDL, "JFG-Launcher.exe": LAUNCHER}


class CopyPlanTests(unittest.TestCase):
    def test_first_use_copies_every_file_as_missing(self) -> None:
        plan = plan_copies(SOURCES, {})
        self.assertEqual([(copy.name, copy.reason) for copy in plan],
                         [("jfg-native-boot.exe", "missing in bundle"), ("SDL2.dll", "missing in bundle"),
                          ("JFG-Launcher.exe", "missing in bundle")])

    def test_nothing_changed_copies_nothing(self) -> None:
        self.assertEqual(plan_copies(SOURCES, dict(SOURCES)), [])

    def test_changed_exe_is_the_only_copy(self) -> None:
        plan = plan_copies({**SOURCES, "jfg-native-boot.exe": NEW_EXE}, dict(SOURCES))
        self.assertEqual([(copy.name, copy.reason) for copy in plan], [("jfg-native-boot.exe", "changed")])

    def test_rebuilt_launcher_is_copied(self) -> None:
        plan = plan_copies({**SOURCES, "JFG-Launcher.exe": NEW_LAUNCHER}, dict(SOURCES))
        self.assertEqual([copy.name for copy in plan], ["JFG-Launcher.exe"])

    def test_file_missing_in_bundle_is_copied(self) -> None:
        bundle = {name: facts for name, facts in SOURCES.items() if name != "SDL2.dll"}
        self.assertEqual([(copy.name, copy.reason) for copy in plan_copies(SOURCES, bundle)],
                         [("SDL2.dll", "missing in bundle")])


class BundleDirectoryTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.jfg = Path(temporary.name) / "JFG"
        (self.jfg / "p").mkdir(parents=True)

    def test_missing_folder_under_the_jfg_root_is_accepted(self) -> None:
        self.assertIsNone(bundle_directory_problem(self.jfg / "p" / "dev", self.jfg))

    def test_empty_folder_is_accepted(self) -> None:
        (self.jfg / "p" / "dev").mkdir()
        self.assertIsNone(bundle_directory_problem(self.jfg / "p" / "dev", self.jfg))

    def test_existing_development_bundle_is_accepted(self) -> None:
        bundle = self.jfg / "p" / "dev"
        bundle.mkdir()
        (bundle / "dev-bundle.json").write_text("{}")
        (bundle / "jfg-package.json").write_text("{}")
        self.assertIsNone(bundle_directory_problem(bundle, self.jfg))

    def test_versioned_bundle_is_refused(self) -> None:
        bundle = self.jfg / "p" / "1.4.1-beta.6"
        bundle.mkdir()
        (bundle / "jfg-package.json").write_text("{}")
        self.assertIn("versioned bundle", bundle_directory_problem(bundle, self.jfg))

    def test_other_non_empty_folder_is_refused(self) -> None:
        (self.jfg / "g").mkdir()
        (self.jfg / "g" / "store-entry.json").write_text("{}")
        self.assertIn("not a development bundle", bundle_directory_problem(self.jfg / "g", self.jfg))

    def test_folder_outside_the_jfg_root_is_refused(self) -> None:
        self.assertIn("must be under", bundle_directory_problem(self.jfg.parent / "elsewhere", self.jfg))

    def test_the_jfg_root_itself_is_refused(self) -> None:
        self.assertIn("must be under", bundle_directory_problem(self.jfg, self.jfg))

    def test_a_file_is_refused(self) -> None:
        (self.jfg / "p" / "dev").write_text("")
        self.assertIn("not a folder", bundle_directory_problem(self.jfg / "p" / "dev", self.jfg))


class VcRuntimeFolderTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.install = Path(temporary.name) / "BuildTools"
        version_file = self.install / "VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt"
        version_file.parent.mkdir(parents=True)
        version_file.write_text("14.44.35112\n")

    def test_finds_the_crt_folder_of_the_default_redist_version(self) -> None:
        crt = self.install / "VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT"
        crt.mkdir(parents=True)
        (self.install / "VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.OpenMP").mkdir()
        self.assertEqual(vc_runtime_folder(self.install), crt)

    def test_missing_crt_folder_names_the_path_tried(self) -> None:
        with self.assertRaisesRegex(ValueError, "14.44.35112"):
            vc_runtime_folder(self.install)

    def test_missing_version_file_names_it(self) -> None:
        (self.install / "VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt").unlink()
        with self.assertRaisesRegex(ValueError, "VCRedistVersion"):
            vc_runtime_folder(self.install)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_identity(executable: Path, *, dirty: bool = True) -> None:
    executable.with_name("jfg-native-boot.exe.support").write_text(
        f"build_source={'1' * 40}\nbuild_dirty={int(dirty)}\nbuild_runtime_sha256={sha256(executable)}\n",
        encoding="ascii")


def assert_launcher_accepts(test: unittest.TestCase, bundle: Path) -> None:
    """The checks PortablePackage.Resolve makes on jfg-package.json and the files it names."""
    manifest = json.loads((bundle / "jfg-package.json").read_text(encoding="utf-8"))
    test.assertEqual(manifest["schema"], 1)
    test.assertTrue(manifest["version"].strip())
    test.assertEqual(sorted(entry["name"] for entry in manifest["files"]), sorted(RUNTIME_FILES))
    for entry in manifest["files"]:
        member = bundle / entry["name"]
        test.assertGreater(entry["size"], 0)
        test.assertRegex(entry["sha256"], re.compile(r"\A[0-9a-f]{64}\Z"))
        test.assertEqual(member.stat().st_size, entry["size"])
        test.assertFalse(member.is_symlink())
        test.assertEqual(sha256(member), entry["sha256"])


class RefreshTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.jfg = root / "JFG"
        self.bundle = self.jfg / "p" / "dev"
        build = root / "build"
        crt = root / "crt"
        launcher_folder = self.jfg / "l"
        for folder in (build, crt, launcher_folder):
            folder.mkdir(parents=True)
        for name in GAME_FILES:
            (build / name).write_bytes(b"MZ game " + name.encode())
        for name in VC_RUNTIME_FILES:
            (crt / name).write_bytes(b"MZ crt " + name.encode())
        (build / "jfg-native-boot.pdb").write_bytes(b"symbols")
        self.executable = build / "jfg-native-boot.exe"
        write_identity(self.executable)
        launcher = launcher_folder / "JFG-Launcher.exe"
        launcher.write_bytes(b"MZ launcher")
        self.sources = BundleSources(executable=self.executable, vc_runtime=crt, launcher=launcher,
                                     checkout=root / "checkout")

    def refresh(self, **options):
        return refresh_dev_bundle(self.bundle, self.jfg, self.sources, **options)

    def rebuild_executable(self) -> None:
        self.executable.write_bytes(b"MZ game rebuilt")
        write_identity(self.executable)

    def test_first_use_creates_a_bundle_the_launcher_accepts_from_a_dirty_runtime(self) -> None:
        result = self.refresh()
        names = sorted(path.name for path in self.bundle.iterdir())
        self.assertEqual(names, sorted([*RUNTIME_FILES, "JFG-Launcher.exe", "jfg-native-boot.exe.support",
                                        "jfg-package.json", "dev-bundle.json"]))
        self.assertEqual(len(result.copied), 16)
        assert_launcher_accepts(self, self.bundle)
        marker = json.loads((self.bundle / "dev-bundle.json").read_text(encoding="utf-8"))
        self.assertEqual(marker["build_dirty"], "1")
        self.assertEqual(marker["checkout"], str(self.sources.checkout))
        self.assertIn("never publish", marker["note"])
        self.assertEqual(list(self.jfg.rglob("*.zip")), [])

    def test_unchanged_runtime_copies_nothing(self) -> None:
        self.refresh()
        manifest_written = (self.bundle / "jfg-package.json").stat().st_mtime_ns
        result = self.refresh()
        self.assertEqual(result.copied, [])
        self.assertFalse(result.manifest_written)
        self.assertEqual((self.bundle / "jfg-package.json").stat().st_mtime_ns, manifest_written)

    def test_rebuilt_runtime_copies_only_the_exe_and_its_identity(self) -> None:
        self.refresh()
        self.rebuild_executable()
        result = self.refresh()
        self.assertEqual([copy.name for copy in result.copied],
                         ["jfg-native-boot.exe", "jfg-native-boot.exe.support"])
        self.assertTrue(result.manifest_written)
        assert_launcher_accepts(self, self.bundle)
        self.assertEqual((self.bundle / "jfg-native-boot.exe").read_bytes(), b"MZ game rebuilt")

    def test_storage_location_is_neither_created_nor_touched(self) -> None:
        self.refresh()
        self.assertFalse((self.bundle / "storage-location.txt").exists())
        (self.bundle / "storage-location.txt").write_text("portable")
        self.rebuild_executable()
        self.refresh()
        self.assertEqual((self.bundle / "storage-location.txt").read_text(), "portable")

    def test_stale_identity_is_refused_before_copying(self) -> None:
        self.executable.write_bytes(b"MZ game rebuilt without identity")
        with self.assertRaisesRegex(ValueError, "identity"):
            self.refresh()
        self.assertFalse(self.bundle.exists())

    def test_missing_launcher_names_the_build_command(self) -> None:
        self.sources.launcher.unlink()
        with self.assertRaisesRegex(ValueError, "build_launcher.ps1"):
            self.refresh()

    def test_versioned_bundle_is_left_unchanged(self) -> None:
        self.bundle.mkdir(parents=True)
        (self.bundle / "jfg-package.json").write_text("{}")
        with self.assertRaisesRegex(ValueError, "versioned bundle"):
            self.refresh()
        self.assertEqual([path.name for path in self.bundle.iterdir()], ["jfg-package.json"])

    def test_locked_file_fails_with_close_message_and_leaves_no_temporary_files(self) -> None:
        self.refresh()
        self.rebuild_executable()

        manifest_before = (self.bundle / "jfg-package.json").read_bytes()

        def exe_locked(source: Path, destination: Path) -> None:
            if destination.name == "jfg-native-boot.exe":
                raise PermissionError(13, "The process cannot access the file", str(destination))
            os.replace(source, destination)

        with self.assertRaises(BundleLockedError) as raised:
            self.refresh(replace=exe_locked, lock_retry_delays_seconds=(0, 0))
        self.assertIn("jfg-native-boot.exe", str(raised.exception))
        self.assertIn("close the launcher and game running from", str(raised.exception))
        self.assertEqual((self.bundle / "jfg-package.json").read_bytes(), manifest_before)
        self.assertIn("rerun with --bundle", str(raised.exception))
        self.assertEqual([path.name for path in self.bundle.iterdir() if ".tmp" in path.name], [])


class BundleArgumentTests(unittest.TestCase):
    JFG = Path("/local/JFG")

    def parse(self, *extra: str):
        return argument_parser().parse_args(["--rom", "game.z64", *extra])

    def test_bundle_is_off_by_default(self) -> None:
        self.assertIsNone(bundle_location(self.parse(), self.JFG))

    def test_bundle_defaults_to_p_dev_under_the_jfg_root(self) -> None:
        self.assertEqual(bundle_location(self.parse("--bundle"), self.JFG), self.JFG / "p" / "dev")

    def test_bundle_dir_overrides_the_location(self) -> None:
        args = self.parse("--bundle", "--bundle-dir", str(self.JFG / "p" / "dev-06"))
        self.assertEqual(bundle_location(args, self.JFG), self.JFG / "p" / "dev-06")

    def test_bundle_options_without_bundle_are_refused(self) -> None:
        for option in (("--bundle-dir", "x"), ("--launcher", "x.exe"), ("--vc-runtime", "crt")):
            with self.subTest(option=option[0]), self.assertRaisesRegex(ValueError, "--bundle"):
                bundle_location(self.parse(*option), self.JFG)


if __name__ == "__main__":
    unittest.main()
