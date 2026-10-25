#!/usr/bin/env python3
"""Export the video core's RT64 patch as a plain .patch file, or check it.

cmake/rt64_n64_video.cmake stays the source and the only thing this repo
builds with. This script applies its patch sites to the pristine pinned RT64
files (cmake/rt64_n64_video_export.cmake) and diffs before against after, so
other projects can take the same changes with `git apply`:

    python scripts/export_n64_video_rt64_patch.py            # rewrite the .patch
    python scripts/export_n64_video_rt64_patch.py --check    # fail if it is stale

The pinned RT64 checkout comes from --rt64-root, else JFG_RT64_ROOT, else this
checkout's dependencies folder (%LOCALAPPDATA%\\JFG\\b\\<checkout-hash>\\d\\rt64),
which a development or from-ROM build clones.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from build_paths import windows_cache

ROOT = Path(__file__).resolve().parents[1]
PATCH_PATH = ROOT / "src/renderer/n64_video/rt64_n64_video.patch"
EXPORT_SCRIPT = ROOT / "cmake/rt64_n64_video_export.cmake"
LOCK_PATH = ROOT / "dependencies.lock.json"
# Visual Studio ships CMake here when it isn't on PATH.
VISUAL_STUDIO_CMAKE_GLOBS = (
    "C:/Program Files*/Microsoft Visual Studio/*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe",
)
# Every git call works on LF files and must not convert them.
GIT_CONFIG = ("-c", "core.autocrlf=false", "-c", "core.safecrlf=false", "-c", "core.quotepath=false")


def pinned_rt64_commit() -> str:
    """Return the RT64 commit dependencies.lock.json pins."""
    lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    return next(repo["commit"] for repo in lock["repositories"] if repo["id"] == "rt64")


def checkout_revision(path: Path) -> str | None:
    """Return the commit checked out at `path`, or None if it isn't a git checkout."""
    result = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def find_pinned_rt64(checkout: Path = ROOT) -> Path | None:
    """The pinned RT64 checkout to export against, or None if there is none."""
    candidates = []
    if os.environ.get("JFG_RT64_ROOT"):
        candidates.append(Path(os.environ["JFG_RT64_ROOT"]))
    if os.environ.get("LOCALAPPDATA"):
        candidates.append(windows_cache(checkout) / "d" / "rt64")
    pinned = pinned_rt64_commit()
    for candidate in candidates:
        if (candidate / "CMakeLists.txt").exists() and checkout_revision(candidate) == pinned:
            return candidate
    return None


def find_cmake() -> Path | None:
    """Return cmake from PATH or a Visual Studio install, or None if there is none."""
    on_path = shutil.which("cmake")
    if on_path:
        return Path(on_path)
    for pattern in VISUAL_STUDIO_CMAKE_GLOBS:
        matches = sorted(glob.glob(pattern))
        if matches:
            return Path(matches[-1])
    return None


def git(repository: Path, *arguments: str) -> str:
    """Run git with `arguments` in `repository` and return its output; raise if it fails."""
    result = subprocess.run(["git", *GIT_CONFIG, "-C", str(repository), *arguments],
                            capture_output=True, text=True, encoding="utf-8", check=True)
    return result.stdout


def snapshot_tree(repository: Path, files: Path) -> str:
    """Replaces the repository's work tree with `files` and returns its git tree id."""
    for entry in repository.iterdir():
        if entry.name != ".git":
            if entry.is_dir():
                shutil.rmtree(entry)
            else:
                entry.unlink()
    shutil.copytree(files, repository, dirs_exist_ok=True)
    git(repository, "add", "--all")
    return git(repository, "write-tree").strip()


def patch_header(rt64_commit: str) -> str:
    """Return the text the .patch starts with, naming the RT64 commit it applies to."""
    return (
        "Video core RT64 patch (src/renderer/n64_video/), against RT64 "
        f"{rt64_commit}.\n"
        "Generated from cmake/rt64_n64_video.cmake by "
        "scripts/export_n64_video_rt64_patch.py; don't edit it by hand.\n"
        "Apply it in the RT64 checkout with: git apply rt64_n64_video.patch\n"
        "See INTEGRATION.md in this folder.\n\n"
    )


def export_patch(rt64_root: Path, cmake: Path) -> str:
    """The .patch text for the video core's patch sites on pinned RT64."""
    with tempfile.TemporaryDirectory(prefix="n64-video-patch-") as directory:
        work = Path(directory)
        before, after, repository = work / "before", work / "after", work / "repository"
        subprocess.run([str(cmake), f"-DRT64_ROOT={rt64_root.as_posix()}",
                        f"-DBEFORE_DIR={before.as_posix()}", f"-DAFTER_DIR={after.as_posix()}",
                        "-P", str(EXPORT_SCRIPT)], check=True, capture_output=True, text=True)
        repository.mkdir()
        git(repository, "init", "--quiet")
        before_tree = snapshot_tree(repository, before)
        after_tree = snapshot_tree(repository, after)
        diff = git(repository, "diff", "--no-color", "--no-ext-diff", "--no-renames", "--full-index",
                   before_tree, after_tree)
    return patch_header(pinned_rt64_commit()) + diff


def main() -> int:
    """Write or check the .patch; return the process exit code."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail if the .patch differs from a fresh export")
    parser.add_argument("--rt64-root", type=Path, help="the pinned RT64 checkout")
    parser.add_argument("--cmake", type=Path, help="the cmake executable")
    parser.add_argument("--patch", type=Path, default=PATCH_PATH, help="the .patch file to write or check")
    args = parser.parse_args()

    rt64_root = args.rt64_root or find_pinned_rt64()
    if rt64_root is None:
        print(f"No pinned RT64 checkout at {pinned_rt64_commit()}; pass --rt64-root or set JFG_RT64_ROOT.",
              file=sys.stderr)
        return 2
    cmake = args.cmake or find_cmake()
    if cmake is None:
        print("cmake not found; pass --cmake.", file=sys.stderr)
        return 2

    try:
        exported = export_patch(rt64_root, cmake)
    except subprocess.CalledProcessError as error:
        print(f"Export failed: {error.cmd}\n{error.stdout}{error.stderr}", file=sys.stderr)
        return 2

    if args.check:
        committed = args.patch.read_text(encoding="utf-8") if args.patch.exists() else ""
        if committed != exported:
            print(f"{args.patch} is out of date with cmake/rt64_n64_video.cmake; "
                  "run scripts/export_n64_video_rt64_patch.py to regenerate it.", file=sys.stderr)
            return 1
        print(f"{args.patch} matches cmake/rt64_n64_video.cmake against RT64 {rt64_root}.")
        return 0

    args.patch.write_text(exported, encoding="utf-8", newline="\n")
    print(f"Wrote {args.patch}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
