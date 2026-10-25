"""Dependency sets: the pinned dependency checkouts, kept once per set of pins.

Fork-only. Development builds of every checkout and worktree share the
checkouts of the four dependencies in build_from_rom.NAMES under
%LOCALAPPDATA%\\JFG\\d. A dependency set, d\\<key>, holds all four; its key
comes from their id, URL and commit in dependencies.lock.json, so equal pins
give the same set everywhere. A set is published like a generated-code store
entry, by renaming a finished staging folder, so a folder at d\\<key> is
always complete. Nothing in the development build writes into a set.
"""
from __future__ import annotations

from collections.abc import Callable, Sequence
from dataclasses import asdict, dataclass
import hashlib
import json
import logging
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_from_rom import NAMES  # noqa: E402
from generated_code_store import KEY_LENGTH, StoreResult, ensure_store_entry  # noqa: E402
from local_app_data import jfg_root  # noqa: E402

# Only these lock fields decide which checkout a dependency is; submodules follow from the commit.
KEY_FIELDS = ("id", "url", "commit")
DEPENDENCY_SETS_NAME = "d"
# The options build_from_rom.py has always cloned with: byte-exact files, and RT64's long paths.
GIT_CLONE_OPTIONS = ("core.autocrlf=false", "core.longpaths=true")

log = logging.getLogger("dependency_set")


@dataclass(frozen=True)
class PinnedDependency:
    """One of the four dependencies, as dependencies.lock.json pins it."""

    id: str
    url: str
    commit: str

    @property
    def folder_name(self) -> str:
        """Return the dependency's folder inside a set, the name build_from_rom.py uses."""
        return NAMES[self.id]


def pinned_dependencies(lock_file: Path) -> list[PinnedDependency]:
    """Return the lock file's pins of the four dependencies, in lock-file order."""
    lock = json.loads(lock_file.read_text(encoding="utf-8"))
    return [PinnedDependency(id=entry["id"], url=entry["url"], commit=entry["commit"])
            for entry in lock["repositories"] if entry["id"] in NAMES]


def key_fields(pin: PinnedDependency) -> dict[str, str]:
    """Return the fields of a pin that go into the dependency-set key."""
    fields = asdict(pin)
    return {name: fields[name] for name in KEY_FIELDS}


def dependency_set_key(lock_file: Path) -> str:
    """Return the dependency-set key for a checkout's lock file.

    Takes the lock file rather than a checkout root, so a build of another
    checkout keys from that checkout's pins.
    """
    pins = sorted(pinned_dependencies(lock_file), key=lambda pin: pin.id)
    document = [key_fields(pin) for pin in pins]
    canonical = json.dumps(document, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:KEY_LENGTH]


def dependency_sets_root(jfg_base: Path | None = None) -> Path:
    """Return the folder of the dependency sets shared by every checkout: %LOCALAPPDATA%\\JFG\\d."""
    if jfg_base is None:
        jfg_base = jfg_root()
    return jfg_base.resolve() / DEPENDENCY_SETS_NAME


Clone = Callable[[PinnedDependency, Path], None]
ReadHead = Callable[[Path], "str | None"]


def dependency_set_problem(entry: Path, pins: Sequence[PinnedDependency], read_head: ReadHead) -> str | None:
    """Return why a folder is not a usable set for the pins, or None when it is."""
    for pin in pins:
        checkout = entry / pin.folder_name
        if not checkout.is_dir():
            return pin.folder_name + " is missing"
        head = read_head(checkout)
        if head != pin.commit:
            return f"{pin.folder_name} is at {head or 'no revision'}, not the pinned {pin.commit}"
    return None


def ensure_dependency_set(sets_root: Path, lock_file: Path, *, clone: Clone, read_head: ReadHead) -> StoreResult:
    """Return a valid dependency set for the lock file's pins, cloning and publishing it when needed.

    clone checks one pinned dependency out, submodules included, into a new
    folder; read_head returns a checkout's HEAD commit, or None. A failed or
    interrupted clone leaves only its staging folder, which is never used.
    """
    pins = pinned_dependencies(lock_file)
    key = dependency_set_key(lock_file)

    def clone_all(staging: Path) -> None:
        started = time.monotonic()
        for pin in pins:
            clone(pin, staging / pin.folder_name)
        log.info("Cloned dependency set %s in %.1f s", key, time.monotonic() - started)

    return ensure_store_entry(sets_root, key, clone_all, lambda entry: dependency_set_problem(entry, pins, read_head))


RunCommand = Callable[[str, list[str]], None]


def git_config_arguments(flag: str) -> list[str]:
    """Return GIT_CLONE_OPTIONS as git config arguments, each option after flag (--config or -c)."""
    arguments = []
    for option in GIT_CLONE_OPTIONS:
        arguments += [flag, option]
    return arguments


def git_clone(run_command: RunCommand) -> Clone:
    """Return a Clone that clones, pins and updates submodules recursively with run_command(name, command)."""
    def clone(pin: PinnedDependency, destination: Path) -> None:
        run_command("clone-" + pin.id, ["git", "clone", *git_config_arguments("--config"), "--no-checkout", pin.url,
                                        str(destination)])
        run_command("pin-" + pin.id, ["git", "-C", str(destination), "checkout", "--detach", pin.commit])
        run_command("submodules-" + pin.id, ["git", *git_config_arguments("-c"), "-C", str(destination), "submodule",
                                             "update", "--init", "--recursive"])
    return clone


def git_output(checkout: Path, *arguments: str) -> str | None:
    """Return the stripped output of a git command run in a checkout, or None when git fails."""
    completed = subprocess.run(["git", "-C", str(checkout), *arguments], capture_output=True, text=True,
                               errors="replace", check=False)
    return completed.stdout.strip() if completed.returncode == 0 else None


def git_head(checkout: Path) -> str | None:
    """Return a checkout's HEAD commit, or None when git can't read it."""
    return git_output(checkout, "rev-parse", "HEAD")
