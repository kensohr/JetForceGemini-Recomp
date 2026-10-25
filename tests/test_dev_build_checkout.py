"""dev_build.py --checkout: building a checkout other than the script's own."""
from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from pathlib import Path

from scripts.dependency_set import dependency_set_key
from scripts.dev_build import (
    DEV_CACHE_FILE,
    ROOT,
    GeneratedCode,
    GenerateSettings,
    argument_parser,
    audit_stamp_warning,
    check_target_checkout,
    configure_arguments,
    dev_workspace,
    generator_input_paths,
    run_generator,
    target_layout,
)

SHA256_OF_ABC = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
CACHE = Path("/" + "x" * 32 + "/JFG/b")
BUILD = Path("/cache/dev/n")
DEPENDENCIES = Path("/d/0123456789abcdef")
NINJA = Path("/vs/ninja.exe")
GENERATED = GeneratedCode(normalized=Path("/g/k/normalized"), libultra=Path("/g/k/libultra.json"),
                          audio=Path("/g/k/audio"))
# A line of the target's GeneratedCode.cmake once it has the audit stamp (commit 0acc320).
STAMP_LINE = 'set(audit_stamp "${audit_directory}/object-shape-audit-$<CONFIG>.stamp")\n'


def make_checkout(root: Path, *, with_stamp: bool = True) -> Path:
    """Create a minimal checkout of this project: top-level CMake, lock file, generator inputs."""
    (root / "CMakeLists.txt").write_text("project(jfg)\n", encoding="utf-8")
    for relative in generator_input_paths(ROOT):
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / relative, destination)
    (root / "cmake").mkdir(exist_ok=True)
    audit = STAMP_LINE if with_stamp else "add_custom_target(${name}_object_shape_audit ALL)\n"
    (root / "cmake/GeneratedCode.cmake").write_text(audit, encoding="utf-8")
    return root


def change_a_pin(lock_file: Path) -> None:
    lock = json.loads(lock_file.read_text(encoding="utf-8"))
    lock["repositories"][0]["commit"] = "f" * 40
    lock_file.write_text(json.dumps(lock), encoding="utf-8")


def option_value(arguments: list[str], option: str) -> str:
    return arguments[arguments.index(option) + 1]


class TargetCheckoutTestCase(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.target = make_checkout(Path(directory.name).resolve())


class CheckoutOptionTests(TargetCheckoutTestCase):
    def test_defaults_to_the_scripts_own_checkout(self) -> None:
        self.assertEqual(argument_parser().parse_args(["--rom", "game.z64"]).checkout, ROOT)

    def test_is_resolved_to_an_absolute_path(self) -> None:
        arguments = argument_parser().parse_args(["--rom", "game.z64", "--checkout", str(self.target)])
        self.assertEqual(arguments.checkout, self.target)
        self.assertTrue(arguments.checkout.is_absolute())

    def test_help_explains_the_option(self) -> None:
        help_text = argument_parser().format_help()
        self.assertIn("--checkout", help_text)
        self.assertIn("default: this script's checkout", " ".join(help_text.split()))


class ConfigureArgumentsTests(TargetCheckoutTestCase):
    def test_source_comes_from_the_target_and_the_cache_file_from_the_script(self) -> None:
        arguments = configure_arguments(BUILD, GENERATED, DEPENDENCIES, NINJA, checkout=self.target)
        self.assertEqual(option_value(arguments, "-S"), str(self.target))
        self.assertEqual(option_value(arguments, "-C"), str(ROOT / "cmake/dev_build.cmake"))

    def test_without_a_checkout_both_are_the_scripts(self) -> None:
        arguments = configure_arguments(BUILD, GENERATED, DEPENDENCIES, NINJA)
        self.assertEqual(option_value(arguments, "-S"), str(ROOT))
        self.assertEqual(option_value(arguments, "-C"), str(DEV_CACHE_FILE))


class GeneratorTests(TargetCheckoutTestCase):
    def generator_script(self, settings: GenerateSettings) -> str:
        commands = []

        class RecordingRunner:
            def run(self, name: str, command: list[str], **options: object) -> None:
                commands.append(command)

        run_generator(RecordingRunner(), Path("/store/key.tmp-1"), settings)
        return commands[0][commands[0].index("python3") + 1]

    def settings(self, **checkout: Path) -> GenerateSettings:
        return GenerateSettings(rom=Path("/roms/game.z64"), dependencies=DEPENDENCIES, distro="Ubuntu-24.04",
                                timeout_seconds=60, **checkout)

    def test_runs_the_targets_generator(self) -> None:
        script = self.generator_script(self.settings(checkout=self.target))
        self.assertTrue(script.endswith("/" + self.target.name + "/scripts/generate_from_rom.py"), script)

    def test_runs_the_scripts_generator_by_default(self) -> None:
        script = self.generator_script(self.settings())
        self.assertTrue(script.endswith("/" + ROOT.name + "/scripts/generate_from_rom.py"), script)


class TargetLayoutTests(TargetCheckoutTestCase):
    def test_workspace_is_the_targets(self) -> None:
        layout = target_layout(self.target, CACHE)
        self.assertEqual(layout.workspace, dev_workspace(self.target, CACHE))
        self.assertNotEqual(layout.workspace, dev_workspace(ROOT, CACHE))

    def test_lock_file_and_dependency_set_key_follow_the_targets_pins(self) -> None:
        change_a_pin(self.target / "dependencies.lock.json")
        layout = target_layout(self.target, CACHE)
        self.assertEqual(layout.lock_file, self.target / "dependencies.lock.json")
        self.assertNotEqual(dependency_set_key(layout.lock_file), dependency_set_key(ROOT / "dependencies.lock.json"))

    def test_generator_inputs_are_the_targets_files(self) -> None:
        (self.target / "scripts/generate_from_rom.py").write_bytes(b"abc")
        self.assertEqual(target_layout(self.target, CACHE).generator_inputs["scripts/generate_from_rom.py"],
                         SHA256_OF_ABC)

    def test_without_a_checkout_the_layout_is_the_scripts(self) -> None:
        layout = target_layout(ROOT, CACHE)
        self.assertEqual(layout.workspace, dev_workspace(ROOT, CACHE))
        self.assertEqual(layout.lock_file, ROOT / "dependencies.lock.json")


class CheckTargetCheckoutTests(TargetCheckoutTestCase):
    def test_accepts_a_checkout_of_this_project(self) -> None:
        check_target_checkout(self.target)

    def test_rejects_a_folder_without_the_lock_file_or_top_level_cmake(self) -> None:
        for missing in ("dependencies.lock.json", "CMakeLists.txt"):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                checkout = make_checkout(Path(directory))
                (checkout / missing).unlink()
                with self.assertRaisesRegex(ValueError, "not a checkout of JetForceGemini-Recomp.*" + missing):
                    check_target_checkout(checkout)

    def test_rejects_a_missing_folder(self) -> None:
        with self.assertRaisesRegex(ValueError, "not a checkout"):
            check_target_checkout(self.target / "missing")


class AuditStampWarningTests(unittest.TestCase):
    def warning_for(self, *, with_stamp: bool) -> str | None:
        with tempfile.TemporaryDirectory() as directory:
            return audit_stamp_warning(make_checkout(Path(directory), with_stamp=with_stamp))

    def test_warns_for_a_target_without_the_stamp(self) -> None:
        warning = self.warning_for(with_stamp=False)
        self.assertIsNotNone(warning)
        self.assertIn("object-shape audit", warning)
        self.assertIn("every build", warning)
        self.assertIn("0acc320", warning)

    def test_is_silent_for_a_target_with_the_stamp(self) -> None:
        self.assertIsNone(self.warning_for(with_stamp=True))

    def test_is_silent_for_the_scripts_own_checkout(self) -> None:
        self.assertIsNone(audit_stamp_warning(ROOT))


if __name__ == "__main__":
    unittest.main()
