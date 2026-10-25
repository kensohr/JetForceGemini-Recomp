"""Generated-code store: generated code kept once per ROM and generator inputs.

Fork-only. Every checkout and worktree shares one store under
%LOCALAPPDATA%\\JFG\\g. A store entry, g\\<key>, holds what a build compiles
from the generated code; its key comes from the ROM SHA-1 and the
generator-input fingerprint, so equal inputs give the same entry everywhere.
An entry is published by renaming a finished staging folder to its final
name, so a folder at g\\<key> is always complete.
"""
from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
from enum import Enum
import hashlib
import json
import logging
import os
from pathlib import Path
import secrets
import shutil
import stat
import sys
import time
from datetime import datetime, timezone

KEY_LENGTH = 16
STAGING_MARK = ".tmp-"
MOVED_ASIDE_MARK = ".bad-"
# Staging and moved-aside folders younger than this may belong to a running generation.
LEFTOVER_MAX_AGE_SECONDS = 24 * 3600
# An antivirus scanner or the indexer can briefly hold a handle in a fresh folder.
RENAME_ATTEMPTS = 5
RENAME_RETRY_DELAY_SECONDS = 1.0

ENTRY_RECORD_NAME = "store-entry.json"
GENERATION_RECORD_NAME = "generation.json"
# What a build needs from generate_from_rom.py's workspace; everything else is an intermediate.
ENTRY_CONTENTS = frozenset({"normalized", "audio", "libultra.json", GENERATION_RECORD_NAME, ENTRY_RECORD_NAME})
ENTRY_LOG_SUFFIX = ".log"
# The files generate_from_rom.py lists in generation.json; the same set that
# build_from_rom.py --resume-native requires.
GENERATED_FILES = frozenset({
    "normalized/sources.json", "libultra.json", "audio/production-audio-adapter.cpp",
    "audio/brokered-rsp.hpp", "audio/brokered-jfg-audio-probe.cpp", "audio/jfg-audio-probe.cpp",
    "normalized/jfg_original_timing.h", "normalized/original-timing.json",
    "normalized/jfg_original_timing_fast.h", "normalized/funcs.h",
})

log = logging.getLogger("generated_code_store")


def store_key(rom_sha1: str, generator_inputs: Mapping[str, str]) -> str:
    """Return the store key for a ROM and a generator-input fingerprint.

    The fingerprint names files by repo-relative path, so the key does not
    depend on where the checkout lives.
    """
    document = {"generator_inputs": dict(generator_inputs), "rom_sha1": rom_sha1}
    canonical = json.dumps(document, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:KEY_LENGTH]


def store_entry_path(store: Path, key: str) -> Path:
    """Return the folder of a store entry; only a complete entry is ever found there."""
    return store / key


def finish_staged_entry(staging: Path, key: str, rom_sha1: str, generator_inputs: Mapping[str, str]) -> None:
    """Turn generate_from_rom.py's workspace into an entry: drop the intermediates, write store-entry.json."""
    for item in staging.iterdir():
        if item.name in ENTRY_CONTENTS or item.name.endswith(ENTRY_LOG_SUFFIX):
            continue
        if item.is_dir() and not item.is_symlink():
            remove_tree(item)
        else:
            item.unlink()
    record = {"key": key, "rom_sha1": rom_sha1, "generator_inputs": dict(generator_inputs),
              "created": datetime.now(timezone.utc).isoformat(timespec="seconds")}
    (staging / ENTRY_RECORD_NAME).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def entry_problem(entry: Path, key: str, rom_sha1: str) -> str | None:
    """Return why a folder is not a usable entry for the key, or None when it is."""
    record = read_json(entry / ENTRY_RECORD_NAME)
    if record is None:
        return "no " + ENTRY_RECORD_NAME
    if record.get("key") != key:
        return ENTRY_RECORD_NAME + " names another key"
    generation = read_json(entry / GENERATION_RECORD_NAME)
    if generation is None:
        return "no " + GENERATION_RECORD_NAME
    return generation_problem(generation, generated_file_digests(entry, generation), rom_sha1)


def generated_file_digests(folder: Path, generation: Mapping[str, object]) -> dict[str, str | None]:
    """Return the SHA-256 of each file generation.json lists, as found on disk (None: missing)."""
    recorded_files = generation.get("files")
    if not isinstance(recorded_files, dict):
        return {}
    # Hash only the known names, so generation.json can't point outside the folder.
    return {name: file_sha256(folder / name) for name in GENERATED_FILES & set(recorded_files)}


def generation_problem(generation: Mapping[str, object], digests: Mapping[str, str | None],
                       rom_sha1: str) -> str | None:
    """Return why generated code can't be used, with the checks of build_from_rom.py --resume-native."""
    if generation.get("complete") is not True:
        return GENERATION_RECORD_NAME + " is incomplete"
    if generation.get("rom_sha1") != rom_sha1:
        return "generated from another ROM"
    recorded_files = generation.get("files")
    if not isinstance(recorded_files, dict) or set(recorded_files) != GENERATED_FILES:
        return GENERATION_RECORD_NAME + " lists unexpected files"
    changed = sorted(name for name, digest in recorded_files.items() if digests.get(name) != digest)
    if changed:
        return "generated files missing or changed: " + ", ".join(changed)
    return None


def file_sha256(path: Path) -> str | None:
    """Return the SHA-256 of a file, or None when it is missing."""
    if not path.is_file():
        return None
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_json(path: Path) -> dict | None:
    """Return a JSON object from a file, or None when it is missing or unreadable."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


class StoreOutcome(Enum):
    """How a run got its store entry."""

    REUSED = "reused"
    GENERATED = "generated"
    REPLACED = "replaced"
    # Another run published the entry while this run was generating it.
    LOST_RACE = "lost-race"


@dataclass(frozen=True)
class StoreResult:
    """The store entry a build uses, and how this run got it."""

    path: Path
    outcome: StoreOutcome
    # Why an existing entry was replaced; None otherwise.
    reason: str | None = None


def staging_folder(store: Path, key: str) -> Path:
    """Return a new, unique staging folder name for an entry; nothing at this name is ever used."""
    return store / f"{key}{STAGING_MARK}{secrets.token_hex(4)}"


def move_aside(entry: Path, problem: str) -> Path:
    """Rename an unusable entry to g\\<key>.bad-<random>, keeping it for diagnosis; never delete it in place."""
    moved = entry.with_name(f"{entry.name}{MOVED_ASIDE_MARK}{secrets.token_hex(4)}")
    log.warning("Store entry %s is unusable (%s); moving it aside to %s", entry.name, problem, moved.name)
    os.rename(entry, moved)
    return moved


def ensure_store_entry(store: Path, key: str, generate: Callable[[Path], None],
                       validate: Callable[[Path], str | None], *, regenerate: bool = False) -> StoreResult:
    """Return a valid store entry for the key, generating and publishing it when needed.

    generate fills an empty staging folder; validate returns why a folder is
    not a usable entry, or None. A failed or interrupted generation leaves
    only its staging folder, which is never used.
    """
    entry = store_entry_path(store, key)
    replace_reason = None
    if entry.is_dir():
        replace_reason = "--regenerate given" if regenerate else validate(entry)
        if replace_reason is None:
            return StoreResult(entry, StoreOutcome.REUSED)
        move_aside(entry, replace_reason)
    store.mkdir(parents=True, exist_ok=True)
    staging = staging_folder(store, key)
    staging.mkdir()
    generate(staging)
    staging_problem = validate(staging)
    if staging_problem:
        raise RuntimeError(f"Generated code in {staging} is unusable ({staging_problem}); it was not published.")
    if not publish(staging, entry):
        log.info("Store entry %s was published by another run meanwhile; discarding %s", key, staging.name)
        remove_tree(staging)
        lost_race_problem = validate(entry)
        if lost_race_problem:
            raise RuntimeError(f"Store entry {entry} published by another run is unusable ({lost_race_problem}).")
        return StoreResult(entry, StoreOutcome.LOST_RACE)
    outcome = StoreOutcome.REPLACED if replace_reason else StoreOutcome.GENERATED
    return StoreResult(entry, outcome, replace_reason)


def publish(staging: Path, entry: Path) -> bool:
    """Rename a finished staging folder to the entry; False when the entry already exists.

    The rename is the commit point of an entry.
    """
    for attempt in range(1, RENAME_ATTEMPTS + 1):
        if entry.exists():
            return False
        try:
            os.rename(staging, entry)
            return True
        except OSError as error:
            if entry.exists():
                return False
            if not isinstance(error, PermissionError) or attempt == RENAME_ATTEMPTS:
                raise
            log.warning("Publishing %s was denied (attempt %d of %d); retrying", entry.name, attempt,
                        RENAME_ATTEMPTS)
            time.sleep(RENAME_RETRY_DELAY_SECONDS)
    return False


def is_leftover(path: Path) -> bool:
    """Return whether a store folder is a staging or moved-aside folder rather than an entry."""
    return path.is_dir() and (STAGING_MARK in path.name or MOVED_ASIDE_MARK in path.name)


def remove_stale_leftovers(store: Path, now: float | None = None,
                           max_age_seconds: float = LEFTOVER_MAX_AGE_SECONDS) -> list[Path]:
    """Delete staging and moved-aside folders older than max_age_seconds; return what was deleted.

    Younger ones are left alone, because a concurrent generation may own them.
    """
    if not store.is_dir():
        return []
    now = time.time() if now is None else now
    removed = []
    for path in sorted(store.iterdir()):
        if not is_leftover(path):
            continue
        age_seconds = now - path.stat().st_mtime
        if age_seconds < max_age_seconds:
            continue
        log.info("Deleting stale store folder %s (%.0f h old)", path.name, age_seconds / 3600)
        remove_tree(path)
        removed.append(path)
    return removed


def remove_tree(path: Path) -> None:
    """Delete a folder, including read-only files."""
    def make_writable_and_retry(function, failed_path, _error) -> None:
        os.chmod(failed_path, stat.S_IWRITE)
        function(failed_path)
    if not path.is_dir():
        return
    # onerror is deprecated from Python 3.12 on; onexc replaces it.
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=make_writable_and_retry)
    else:
        shutil.rmtree(path, onerror=make_writable_and_retry)
