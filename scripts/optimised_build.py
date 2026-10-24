"""Optimised build: the from-ROM build with the release optimisations of
official releases (cmake/native_release.cmake).

Its pinned generators accept only generated code with the native timing spans
applied, so generation must apply them, and a workspace's mode is fixed once
generated: `normalized/original-timing.json` gains a `native_spans` key.
"""
from __future__ import annotations

import json
from pathlib import Path

NATIVE_SPANS_ARGUMENT = "--native-spans"
# Kept apart from the default "n" folder so optimised and unoptimised objects
# never mix; one letter because MSBuild paths must stay short.
BUILD_FOLDER = "o"


def announce(*, is_enabled: bool) -> None:
    if is_enabled:
        print("Optimised build: native timing spans and cmake/native_release.cmake", flush=True)


def generate_arguments(*, is_enabled: bool) -> list[str]:
    return [NATIVE_SPANS_ARGUMENT] if is_enabled else []


def configure_arguments(root: Path, *, is_enabled: bool) -> list[str]:
    return ["-C", str(root / "cmake/native_release.cmake")] if is_enabled else []


def check_resume(workspace: Path, *, is_enabled: bool) -> None:
    """Refuse to resume a workspace generated in the other mode."""
    record = json.loads((workspace / "normalized/original-timing.json").read_text())
    has_native_spans = "native_spans" in record
    if has_native_spans == is_enabled:
        return
    if has_native_spans:
        raise ValueError("Workspace was generated for an optimised build; "
                         "resume with --release-optimizations or start a new build.")
    raise ValueError("Workspace was generated without release optimisations; "
                     "start a new build with --release-optimizations.")
