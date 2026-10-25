"""Check that the bundled launcher was built from the same launcher sources as the runtime.

Fork-only, used by dev_bundle.py. build_launcher.ps1 writes launcher-build.json
next to the launcher: its source_commit, executable_sha256 and the SHA-256 of
every source file it was built from (inputs). A launcher built on another
branch can write settings the runtime rejects (e.g. a video.ini key), and the
runtime then exits right after rom-ready. Comparing inputs instead of commits
stays silent after runtime-only commits and works for uncommitted changes.
"""
from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
import json
import logging
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_beta import digest  # noqa: E402

LOG = logging.getLogger("dev_bundle_launcher_source")
RECEIPT_NAME = "launcher-build.json"
MALFORMED_REASON = f"{RECEIPT_NAME} is malformed"
# The receipt fields the comparison needs, with the JSON type each must have.
REQUIRED_FIELDS = {"source_commit": str, "executable_sha256": str, "inputs": dict}
LAUNCHER_BUILD_COMMAND = (r"powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_launcher.ps1 "
                          r"-OutputDirectory $env:LOCALAPPDATA\JFG\l")
# The warning lists this many differing files; a launcher from another branch can differ in dozens.
MAX_LISTED_INPUTS = 5

# Returns the SHA-256 of a checkout file by its receipt path, or None when it is missing.
CheckoutDigest = Callable[[str], "str | None"]


@dataclass(frozen=True)
class LauncherSource:
    """How the launcher's build receipt compares with the runtime's checkout."""

    source_commit: str | None
    # Receipt paths whose checkout file is missing or has another SHA-256, in receipt order.
    differing_inputs: tuple[str, ...] = ()
    # Why the receipt can't tell where the launcher came from; None when it can.
    unknown_reason: str | None = None


def unknown_source(reason: str) -> LauncherSource:
    """Return the outcome for a receipt that can't tell where the launcher came from."""
    return LauncherSource(source_commit=None, unknown_reason=reason)


def is_well_formed(receipt: object) -> bool:
    """Return whether a parsed receipt has the fields the comparison needs."""
    if not isinstance(receipt, dict):
        return False
    for field, field_type in REQUIRED_FIELDS.items():
        if not isinstance(receipt.get(field), field_type):
            return False
    return True


def compare_launcher_source(receipt: object, launcher_sha256: str, checkout_digest: CheckoutDigest) -> LauncherSource:
    """Compare a parsed launcher-build.json (None when missing) with the checkout the runtime was built from."""
    if receipt is None:
        return unknown_source(f"no {RECEIPT_NAME} next to the launcher")
    if not is_well_formed(receipt):
        return unknown_source(MALFORMED_REASON)
    if receipt["executable_sha256"].lower() != launcher_sha256:
        return unknown_source(f"{RECEIPT_NAME} describes another build of the launcher")
    differing = tuple(path for path, sha256 in receipt["inputs"].items() if checkout_digest(path) != sha256)
    return LauncherSource(source_commit=receipt["source_commit"], differing_inputs=differing)


def launcher_source_warning(source: LauncherSource, identity: Mapping[str, str]) -> str | None:
    """Return the warning to show for a launcher from other or unknown sources, or None when it matches."""
    if source.unknown_reason:
        return (f"The bundled launcher's build source is unknown ({source.unknown_reason}), so it may not match "
                f"the runtime ({runtime_description(identity)}). Rebuild it with: {LAUNCHER_BUILD_COMMAND}")
    if not source.differing_inputs:
        return None
    count = len(source.differing_inputs)
    files = "file differs" if count == 1 else "files differ"
    shown = ", ".join(source.differing_inputs[:MAX_LISTED_INPUTS])
    more = f" and {count - MAX_LISTED_INPUTS} more" if count > MAX_LISTED_INPUTS else ""
    return (f"The bundled launcher was built from other launcher sources ({source.source_commit}) than the runtime "
            f"({runtime_description(identity)}): {count} launcher source {files} ({shown}{more}). "
            f"The launcher may write settings the runtime rejects. Rebuild it with: {LAUNCHER_BUILD_COMMAND}")


def runtime_description(identity: Mapping[str, str]) -> str:
    """Return the runtime's build commit, marked when it was built from uncommitted changes."""
    commit = identity.get("build_source", "unknown commit")
    return f"{commit} with uncommitted changes" if identity.get("build_dirty") == "1" else commit


def read_receipt(path: Path) -> object | None:
    """Return the parsed JSON of a receipt file, or None when it can't be read or isn't JSON."""
    try:
        # build_launcher.ps1 runs under Windows PowerShell, which may write a BOM.
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        LOG.debug("Unreadable launcher receipt %s", path, exc_info=True)
        return None


def receipt_source(receipt_path: Path, launcher_sha256: str, checkout_digest: CheckoutDigest) -> LauncherSource:
    """Compare the receipt file next to the launcher, missing or not, with the runtime's checkout."""
    if not receipt_path.is_file():
        return compare_launcher_source(None, launcher_sha256, checkout_digest)
    receipt = read_receipt(receipt_path)
    if receipt is None:
        return unknown_source(MALFORMED_REASON)
    return compare_launcher_source(receipt, launcher_sha256, checkout_digest)


def checkout_digest_reader(checkout: Path) -> CheckoutDigest:
    """Return a CheckoutDigest that hashes files of a checkout, None for a missing one."""
    def checkout_digest(relative: str) -> str | None:
        path = checkout / relative
        return digest(path) if path.is_file() else None
    return checkout_digest


def check_launcher_source(launcher: Path, checkout: Path, identity: Mapping[str, str]) -> str | None:
    """Return the warning for a launcher built from other or unknown sources than the runtime's checkout."""
    receipt_path = launcher.parent / RECEIPT_NAME
    source = receipt_source(receipt_path, digest(launcher), checkout_digest_reader(checkout))
    if source.unknown_reason is None and not source.differing_inputs:
        LOG.debug("Launcher sources match the runtime checkout %s (launcher %s, runtime %s)", checkout,
                  source.source_commit, identity.get("build_source"))
    return launcher_source_warning(source, identity)
