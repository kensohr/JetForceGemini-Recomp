"""The video core's RT64 .patch matches what its CMake patch script makes.

Needs the pinned RT64 checkout: JFG_RT64_ROOT, or this checkout's
dependencies folder from a development or from-ROM build. Skips without it.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import export_n64_video_rt64_patch as export  # noqa: E402


class N64VideoRt64PatchTests(unittest.TestCase):
    def setUp(self):
        self.rt64_root = export.find_pinned_rt64(ROOT)
        if self.rt64_root is None:
            self.skipTest("no pinned RT64 checkout (set JFG_RT64_ROOT)")
        self.cmake = export.find_cmake()
        if self.cmake is None:
            self.skipTest("cmake not found")

    def test_committed_patch_matches_the_cmake_patch_script(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "scripts/export_n64_video_rt64_patch.py"), "--check",
             "--rt64-root", str(self.rt64_root), "--cmake", str(self.cmake)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_check_fails_when_the_committed_patch_differs(self):
        committed = export.PATCH_PATH.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            stale = Path(directory) / "stale.patch"
            stale.write_text(committed.replace("VI_CRT", "VI_CRT_STALE", 1), encoding="utf-8", newline="\n")
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/export_n64_video_rt64_patch.py"), "--check",
                 "--rt64-root", str(self.rt64_root), "--cmake", str(self.cmake), "--patch", str(stale)],
                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("out of date", result.stderr)


if __name__ == "__main__":
    unittest.main()
