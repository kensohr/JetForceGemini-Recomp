"""Generated-source preparation leaves unchanged outputs untouched.

Every CMake configure reruns the preparation. Its per-name
symbol-inventory.json copy is an input of the object-shape audit, so
rewriting an unchanged file would rerun the audit after each reconfigure.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

import test_generated_source_preparation as preparation_tests

from scripts.prepare_generated_sources import prepare


OLD_MTIME_NS = 1_000_000_000 * 1_000_000_000


class UnchangedPreparationOutputTests(unittest.TestCase):
    # Reuse the fixture roots of the existing preparation tests.
    make_root = preparation_tests.GeneratedSourcePreparationTests.make_root
    make_v3_root = preparation_tests.GeneratedSourcePreparationTests.make_v3_root

    def age_outputs(self, output: Path) -> list[Path]:
        outputs = sorted(path for path in output.iterdir() if path.is_file())
        for path in outputs:
            os.utime(path, ns=(OLD_MTIME_NS, OLD_MTIME_NS))
        return outputs

    def replace_symbol_inventory(self, root: Path, inventory: bytes) -> None:
        (root / "symbol_inventory.json").write_bytes(inventory)
        manifest_path = root / "sources.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["symbol_inventory_sha256"] = hashlib.sha256(inventory).hexdigest()
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    def test_rerun_with_unchanged_inputs_keeps_every_output_timestamp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            self.make_v3_root(root)
            output = root / "output"
            prepare(root, output, require_normalizer_revision=True)
            outputs = self.age_outputs(output)
            contents_before = {path.name: path.read_bytes() for path in outputs}

            prepare(root, output, require_normalizer_revision=True)

            self.assertIn(output / "symbol-inventory.json", outputs)
            self.assertIn(output / "cpu-section-inventory.json", outputs)
            for path in outputs:
                with self.subTest(output=path.name):
                    self.assertEqual(path.stat().st_mtime_ns, OLD_MTIME_NS)
                    self.assertEqual(path.read_bytes(), contents_before[path.name])

    def test_changed_inventory_is_rewritten_with_new_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            self.make_v3_root(root)
            output = root / "output"
            prepare(root, output, require_normalizer_revision=True)
            self.age_outputs(output)
            changed_inventory = b'{"changed":true}\n'
            self.replace_symbol_inventory(root, changed_inventory)

            prepare(root, output, require_normalizer_revision=True)

            inventory_copy = output / "symbol-inventory.json"
            self.assertEqual(inventory_copy.read_bytes(), changed_inventory)
            self.assertNotEqual(inventory_copy.stat().st_mtime_ns, OLD_MTIME_NS)
            unchanged_list = output / "baseline-body.txt"
            self.assertEqual(unchanged_list.stat().st_mtime_ns, OLD_MTIME_NS)


if __name__ == "__main__":
    unittest.main()
