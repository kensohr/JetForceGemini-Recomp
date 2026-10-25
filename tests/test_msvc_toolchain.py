from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scripts.msvc_toolchain import parse_environment_listing, visual_studio_tools


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
