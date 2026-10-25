from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from scripts.dev_bundle import BundleSources, refresh_dev_bundle
from scripts.dev_bundle_launcher_source import compare_launcher_source, launcher_source_warning
from scripts.package_beta import GAME_FILES, VC_RUNTIME_FILES
from tests.test_dev_bundle import assert_launcher_accepts, sha256, write_identity

LAUNCHER_SHA256 = "1" * 64
CHECKOUT = {"launcher/windows/Launcher.cs": "a" * 64, "include/jfg/renderer/video_settings.hpp": "b" * 64}
CLEAN_RUNTIME = {"build_source": "141df5c", "build_dirty": "0"}


def receipt(**overrides) -> dict:
    return {"source_commit": "06713e8", "executable_sha256": LAUNCHER_SHA256, "inputs": dict(CHECKOUT), **overrides}


def warning_for(launcher_receipt, identity=CLEAN_RUNTIME, checkout=CHECKOUT, launcher_sha256=LAUNCHER_SHA256):
    source = compare_launcher_source(launcher_receipt, launcher_sha256, checkout.get)
    return launcher_source_warning(source, identity)


class LauncherSourceTests(unittest.TestCase):
    def test_matching_inputs_give_no_warning_even_when_the_commits_differ(self) -> None:
        self.assertIsNone(warning_for(receipt()))

    def test_changed_input_warns_naming_both_commits_the_file_and_the_rebuild_command(self) -> None:
        checkout = {**CHECKOUT, "include/jfg/renderer/video_settings.hpp": "c" * 64}
        warning = warning_for(receipt(), checkout=checkout)
        self.assertIn("06713e8", warning)
        self.assertIn("141df5c", warning)
        self.assertIn("1 launcher source file differs", warning)
        self.assertIn("include/jfg/renderer/video_settings.hpp", warning)
        self.assertNotIn("launcher/windows/Launcher.cs", warning)
        self.assertNotIn("uncommitted", warning)
        self.assertIn("build_launcher.ps1", warning)

    def test_input_missing_from_the_checkout_counts_as_differing(self) -> None:
        inputs = {**CHECKOUT, "launcher/native/video_output.hpp": "d" * 64}
        warning = warning_for(receipt(inputs=inputs))
        self.assertIn("1 launcher source file differs", warning)
        self.assertIn("launcher/native/video_output.hpp", warning)

    def test_at_most_five_differing_files_are_listed(self) -> None:
        inputs = {f"launcher/ui/page{number}.json": "e" * 64 for number in range(1, 8)}
        warning = warning_for(receipt(inputs=inputs))
        self.assertIn("7 launcher source files differ", warning)
        self.assertIn("launcher/ui/page5.json and 2 more", warning)
        self.assertNotIn("page6", warning)

    def test_dirty_runtime_is_marked_in_the_warning(self) -> None:
        dirty = {"build_source": "141df5c", "build_dirty": "1"}
        warning = warning_for(receipt(inputs={"launcher/native/video_output.hpp": "d" * 64}), identity=dirty)
        self.assertIn("141df5c with uncommitted changes", warning)

    def assert_unknown_source(self, warning: str | None, reason: str) -> None:
        self.assertIn("launcher's build source is unknown", warning)
        self.assertIn(reason, warning)
        self.assertIn("build_launcher.ps1", warning)

    def test_missing_receipt_warns_that_the_source_is_unknown(self) -> None:
        self.assert_unknown_source(warning_for(None), "no launcher-build.json")

    def test_malformed_receipt_warns_that_the_source_is_unknown(self) -> None:
        for name, malformed in (("not an object", ["inputs"]), ("no inputs", {"source_commit": "06713e8",
                                                                             "executable_sha256": LAUNCHER_SHA256}),
                                ("inputs not an object", receipt(inputs=["launcher/windows/Launcher.cs"])),
                                ("no exe digest", {"source_commit": "06713e8", "inputs": dict(CHECKOUT)}),
                                ("no source commit", {"executable_sha256": LAUNCHER_SHA256,
                                                      "inputs": dict(CHECKOUT)}),
                                ("source commit not a string", receipt(source_commit=None))):
            with self.subTest(name):
                self.assert_unknown_source(warning_for(malformed), "launcher-build.json is malformed")

    def test_stale_receipt_warns_that_the_source_is_unknown(self) -> None:
        self.assert_unknown_source(warning_for(receipt(), launcher_sha256="2" * 64),
                                   "launcher-build.json describes another build")


class RefreshWarningTests(unittest.TestCase):
    """refresh_dev_bundle reports a launcher from other sources and still refreshes the bundle."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.jfg = root / "JFG"
        self.bundle = self.jfg / "p" / "dev"
        build, crt, launcher_folder, self.checkout = root / "build", root / "crt", self.jfg / "l", root / "checkout"
        for folder in (build, crt, launcher_folder, self.checkout / "launcher"):
            folder.mkdir(parents=True)
        for name in GAME_FILES:
            (build / name).write_bytes(b"MZ game " + name.encode())
        for name in VC_RUNTIME_FILES:
            (crt / name).write_bytes(b"MZ crt " + name.encode())
        executable = build / "jfg-native-boot.exe"
        write_identity(executable)
        self.launcher = launcher_folder / "JFG-Launcher.exe"
        self.launcher.write_bytes(b"MZ launcher")
        self.launcher_input = self.checkout / "launcher" / "Launcher.cs"
        self.launcher_input.write_text("class Launcher {}")
        self.sources = BundleSources(executable=executable, vc_runtime=crt, launcher=self.launcher,
                                     checkout=self.checkout)

    def write_receipt(self) -> None:
        receipt = {"source_commit": "06713e8", "executable_sha256": sha256(self.launcher),
                   "inputs": {"launcher/Launcher.cs": sha256(self.launcher_input)}}
        (self.launcher.parent / "launcher-build.json").write_text(json.dumps(receipt), encoding="utf-8")

    def refresh(self):
        with self.assertLogs("dev_bundle", level="DEBUG") as logs:
            result = refresh_dev_bundle(self.bundle, self.jfg, self.sources)
        warnings = [record.getMessage() for record in logs.records if record.levelname == "WARNING"]
        return result, warnings

    def test_matching_launcher_sources_give_no_warning(self) -> None:
        self.write_receipt()
        result, warnings = self.refresh()
        self.assertIsNone(result.launcher_warning)
        self.assertEqual(warnings, [])

    def test_changed_launcher_source_warns_and_still_refreshes(self) -> None:
        self.write_receipt()
        self.launcher_input.write_text("class Launcher { VideoOutput output; }")
        result, warnings = self.refresh()
        self.assertIn("launcher/Launcher.cs", result.launcher_warning)
        self.assertIn("06713e8", result.launcher_warning)
        self.assertIn("1" * 40 + " with uncommitted changes", result.launcher_warning)
        self.assertEqual(warnings, [result.launcher_warning])
        assert_launcher_accepts(self, self.bundle)
        self.assertEqual((self.bundle / "JFG-Launcher.exe").read_bytes(), b"MZ launcher")

    def test_missing_receipt_warns_also_when_the_bundle_is_up_to_date(self) -> None:
        self.refresh()
        result, warnings = self.refresh()
        self.assertEqual(result.copied, [])
        self.assertIn("build source is unknown", result.launcher_warning)
        self.assertEqual(warnings, [result.launcher_warning])

    def test_malformed_receipt_warns_and_still_refreshes(self) -> None:
        (self.launcher.parent / "launcher-build.json").write_text("{not json", encoding="utf-8")
        result, _ = self.refresh()
        self.assertIn("launcher-build.json is malformed", result.launcher_warning)
        assert_launcher_accepts(self, self.bundle)


if __name__ == "__main__":
    unittest.main()
