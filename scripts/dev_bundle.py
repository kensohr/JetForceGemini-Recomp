"""Refresh the development bundle: one unpacked launcher bundle for development builds.

Fork-only. dev_build.py --bundle keeps %LOCALAPPDATA%\\JFG\\p\\dev up to date
with the freshly built runtime, so Modern Controls can be tested through the
launcher without a new versioned bundle per test. Unlike package_beta.py it
accepts a runtime built from uncommitted changes (build_dirty=1); that is safe
only because the development bundle is never zipped or published: it contains
code compiled from the ROM.
"""
from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import logging
import os
from pathlib import Path
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_beta import RUNTIME_FILES, digest, runtime_inputs, runtime_manifest  # noqa: E402

LOG = logging.getLogger("dev_bundle")
DEV_MARKER_NAME = "dev-bundle.json"
MANIFEST_NAME = "jfg-package.json"
LAUNCHER_NAME = "JFG-Launcher.exe"
IDENTITY_NAME = "jfg-native-boot.exe.support"
MANIFEST_VERSION = "dev"
TEMPORARY_SUFFIX = ".tmp-dev-bundle"
# A launcher or game that is just closing releases its files within a second or two.
LOCK_RETRY_DELAYS_SECONDS = (0.5, 1.0, 2.0)
LAUNCHER_BUILD_COMMAND = (r"powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_launcher.ps1 "
                          r"-OutputDirectory $env:LOCALAPPDATA\JFG\l")


@dataclass(frozen=True)
class FileFacts:
    """Size and SHA-256 of a bundle file; what the launcher verifies."""

    size: int
    sha256: str


@dataclass(frozen=True)
class PlannedCopy:
    """A file the refresh copies into the bundle, and why."""

    name: str
    reason: str


def plan_copies(sources: Mapping[str, FileFacts], bundle: Mapping[str, FileFacts]) -> list[PlannedCopy]:
    """Return the source files the bundle lacks or holds a different version of, in source order."""
    plan = []
    for name, facts in sources.items():
        held = bundle.get(name)
        if held is None:
            plan.append(PlannedCopy(name, "missing in bundle"))
        elif held != facts:
            plan.append(PlannedCopy(name, "changed"))
    return plan


def bundle_directory_problem(bundle: Path, jfg_root: Path) -> str | None:
    """Return why a folder can't be the development bundle, or None when it can.

    It must be under the JFG root and be missing, empty or already a
    development bundle (holding the dev marker), so a versioned bundle or any
    other folder is never overwritten.
    """
    bundle = bundle.resolve()
    jfg_root = jfg_root.resolve()
    if bundle == jfg_root or not bundle.is_relative_to(jfg_root):
        return f"The development bundle must be under {jfg_root}: {bundle}"
    if not bundle.exists():
        return None
    if not bundle.is_dir():
        return f"The development bundle path is not a folder: {bundle}"
    if (bundle / DEV_MARKER_NAME).is_file() or not any(bundle.iterdir()):
        return None
    if (bundle / MANIFEST_NAME).is_file():
        return f"Refusing to refresh {bundle}: it is a versioned bundle, not a development bundle."
    return f"Refusing to refresh {bundle}: the folder is not empty and not a development bundle."


VC_REDIST_VERSION_FILE = "VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt"
VC_REDIST_FOLDER = "VC/Redist/MSVC"
VC_CRT_PATTERN = "Microsoft.VC*.CRT"


def vc_runtime_folder(install: Path) -> Path:
    """Return the x64 VC CRT redist folder of a Visual Studio install; fail naming the path tried."""
    version_file = install / VC_REDIST_VERSION_FILE
    if not version_file.is_file():
        raise ValueError(f"VC runtime version file missing: {version_file}. Pass --vc-runtime <folder>.")
    version = version_file.read_text(encoding="utf-8-sig").strip()
    x64 = install / VC_REDIST_FOLDER / version / "x64"
    found = sorted(path for path in x64.glob(VC_CRT_PATTERN) if path.is_dir()) if x64.is_dir() else []
    if len(found) != 1:
        raise ValueError(f"Expected one VC runtime folder {x64 / VC_CRT_PATTERN}, found {len(found)}. "
                         "Pass --vc-runtime <folder>.")
    return found[0]


@dataclass(frozen=True)
class BundleSources:
    """Where the files of a refresh come from."""

    # The built runtime; SDL2.dll, the dx DLLs and the .support identity lie next to it.
    executable: Path
    vc_runtime: Path
    launcher: Path
    # The checkout the runtime was built from, recorded in the dev marker.
    checkout: Path


@dataclass(frozen=True)
class RefreshResult:
    """What a refresh changed in the development bundle."""

    bundle: Path
    copied: list[PlannedCopy]
    manifest_written: bool


class BundleLockedError(RuntimeError):
    """A bundle file stayed locked, usually by the launcher or game running from the bundle."""


Replace = Callable[[Path, Path], None]


def read_identity(identity: Path) -> dict[str, str]:
    """Return the key=value lines of a runtime's .support identity."""
    if not identity.is_file():
        raise ValueError(f"Runtime build identity missing: {identity}")
    lines = identity.read_text(encoding="ascii").splitlines()
    return dict(line.split("=", 1) for line in lines if "=" in line)


def file_facts(path: Path) -> FileFacts:
    return FileFacts(size=path.stat().st_size, sha256=digest(path))


@dataclass(frozen=True)
class CheckedSources:
    """The source files of a refresh, checked and described."""

    # jfg-package.json for the runtime with version "dev": what the bundle's manifest becomes.
    manifest: dict
    paths: dict[str, Path]
    facts: dict[str, FileFacts]
    identity: dict[str, str]


def check_sources(sources: BundleSources) -> CheckedSources:
    """Check and describe the source files; fail before anything is copied.

    The identity must describe this exe. A dirty identity (build_dirty=1) is
    accepted, unlike in package_beta.py, because the bundle is never published.
    """
    if not sources.launcher.is_file():
        raise ValueError(f"Launcher missing: {sources.launcher}. Build it with: {LAUNCHER_BUILD_COMMAND}")
    runtime = sources.executable.parent
    manifest = runtime_manifest(runtime, MANIFEST_VERSION, sources.vc_runtime)
    identity_path = runtime / IDENTITY_NAME
    identity = read_identity(identity_path)
    if identity.get("build_runtime_sha256") != manifest["files"][0]["sha256"]:
        raise ValueError(f"Stale runtime identity: {identity_path} describes another build of "
                         f"{sources.executable}. Rebuild to rewrite it.")
    paths = dict(runtime_inputs(runtime, sources.vc_runtime))
    facts = {entry["name"]: FileFacts(entry["size"], entry["sha256"]) for entry in manifest["files"]}
    for name, path in ((LAUNCHER_NAME, sources.launcher), (IDENTITY_NAME, identity_path)):
        paths[name] = path
        facts[name] = file_facts(path)
    return CheckedSources(manifest, paths, facts, identity)


def read_manifest_entries(bundle: Path) -> dict[str, FileFacts]:
    """Return the runtime files the bundle's jfg-package.json records; empty when it is missing or unreadable."""
    try:
        manifest = json.loads((bundle / MANIFEST_NAME).read_text(encoding="utf-8"))
        return {entry["name"]: FileFacts(entry["size"], entry["sha256"]) for entry in manifest["files"]}
    except (OSError, ValueError, KeyError, TypeError):
        return {}


def read_bundle_files(bundle: Path) -> dict[str, FileFacts]:
    """Return what the bundle holds, by file name.

    Runtime files are taken from the bundle's manifest when the file is there
    with the recorded size, which saves hashing ~150 MB per run (the launcher
    verifies the digests on every start). The launcher and identity, which the
    manifest doesn't list, are hashed.
    """
    held: dict[str, FileFacts] = {}
    recorded = read_manifest_entries(bundle)
    for name in RUNTIME_FILES:
        member = bundle / name
        entry = recorded.get(name)
        if entry and member.is_file() and not member.is_symlink() and member.stat().st_size == entry.size:
            held[name] = entry
    for name in (LAUNCHER_NAME, IDENTITY_NAME):
        member = bundle / name
        if member.is_file() and not member.is_symlink():
            held[name] = file_facts(member)
    return held


def locked_message(bundle: Path, name: str) -> str:
    return (f"{name} in the development bundle is locked: close the launcher and game running from {bundle} "
            "and rerun with --bundle. Until a refresh succeeds, the launcher there may refuse the bundle.")


def move_into_place(temporary: Path, destination: Path, replace: Replace,
                    retry_delays_seconds: Sequence[float]) -> None:
    """Move a staged file over its bundle file, retrying while the old one is locked."""
    for attempt, delay in enumerate((*retry_delays_seconds, None), start=1):
        try:
            replace(temporary, destination)
            return
        except PermissionError as error:
            if delay is None:
                temporary.unlink(missing_ok=True)
                raise BundleLockedError(locked_message(destination.parent, destination.name)) from error
            LOG.warning("%s is locked (attempt %d); retrying in %s s", destination, attempt, delay)
            time.sleep(delay)


def install_file(bundle: Path, name: str, write: Callable[[Path], None], replace: Replace,
                 retry_delays_seconds: Sequence[float]) -> None:
    """Write a bundle file under a temporary name, then move it over the old one."""
    temporary = bundle / (name + TEMPORARY_SUFFIX)
    write(temporary)
    move_into_place(temporary, bundle / name, replace, retry_delays_seconds)


def json_writer(data: dict) -> Callable[[Path], None]:
    return lambda path: path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def copier(source: Path) -> Callable[[Path], None]:
    return lambda path: shutil.copyfile(source, path)


def dev_marker(sources: BundleSources, identity: Mapping[str, str], refreshed_at: datetime) -> dict:
    """Return the dev-bundle.json marker; its presence is what makes a folder a development bundle."""
    return {"checkout": str(sources.checkout), "build_source": identity.get("build_source"),
            "build_dirty": identity.get("build_dirty"), "refreshed_at": refreshed_at.isoformat(timespec="seconds"),
            "note": "Development bundle: local only, never publish. It contains code compiled from the ROM."}


def refresh_dev_bundle(bundle: Path, jfg_root: Path, sources: BundleSources, *, replace: Replace = os.replace,
                       lock_retry_delays_seconds: Sequence[float] = LOCK_RETRY_DELAYS_SECONDS) -> RefreshResult:
    """Copy the runtime files and launcher that changed into the development bundle.

    The first use (missing or empty folder) copies everything. The dev marker
    is written before the copies, so a half-made bundle is still recognised as
    a development bundle; jfg-package.json is written last. Files the refresh
    doesn't own (e.g. storage-location.txt) are never created or removed.
    """
    problem = bundle_directory_problem(bundle, jfg_root)
    if problem:
        raise ValueError(problem)
    bundle = bundle.resolve()
    checked = check_sources(sources)
    plan = plan_copies(checked.facts, read_bundle_files(bundle))
    LOG.debug("Copy plan for %s: %s", bundle, [(copy.name, copy.reason) for copy in plan])
    if not plan and (bundle / DEV_MARKER_NAME).is_file():
        return RefreshResult(bundle, [], manifest_written=False)

    bundle.mkdir(parents=True, exist_ok=True)
    marker = dev_marker(sources, checked.identity, datetime.now(timezone.utc))
    install_file(bundle, DEV_MARKER_NAME, json_writer(marker), replace, lock_retry_delays_seconds)
    for copy in plan:
        install_file(bundle, copy.name, copier(checked.paths[copy.name]), replace, lock_retry_delays_seconds)
        LOG.info("  copied %s (%s)", copy.name, copy.reason)
    runtime_copied = any(copy.name in RUNTIME_FILES for copy in plan)
    if runtime_copied:
        install_file(bundle, MANIFEST_NAME, json_writer(checked.manifest), replace, lock_retry_delays_seconds)
    return RefreshResult(bundle, plan, manifest_written=runtime_copied)


BUNDLE_FOLDER = Path("p") / "dev"
LAUNCHER_FOLDER = "l"


def add_bundle_arguments(parser: argparse.ArgumentParser) -> None:
    """Add --bundle and its options to dev_build.py's parser."""
    parser.add_argument("--bundle", action="store_true",
                        help=r"After the build, refresh the development bundle (%%LOCALAPPDATA%%\JFG\p\dev) "
                             "to test through the launcher; never publish it")
    parser.add_argument("--bundle-dir", type=Path,
                        help=r"Development bundle folder instead of p\dev (under %%LOCALAPPDATA%%\JFG)")
    parser.add_argument("--launcher", type=Path, help=r"Launcher to bundle instead of JFG\l\JFG-Launcher.exe")
    parser.add_argument("--vc-runtime", type=Path,
                        help="x64 Microsoft.VC*.CRT folder instead of the Visual Studio redist one")


def jfg_root() -> Path:
    r"""Return %LOCALAPPDATA%\JFG, the root all local build output lives under."""
    local = os.environ.get("LOCALAPPDATA")
    if not local:
        raise ValueError("LOCALAPPDATA is unavailable, so the development bundle can't be found.")
    return Path(local) / "JFG"


def bundle_location(args: argparse.Namespace, root: Path) -> Path | None:
    """Return the development bundle a run refreshes, or None without --bundle."""
    if not args.bundle:
        given = [option for option, value in (("--bundle-dir", args.bundle_dir), ("--launcher", args.launcher),
                                              ("--vc-runtime", args.vc_runtime)) if value is not None]
        if given:
            raise ValueError(f"{', '.join(given)} only applies with --bundle.")
        return None
    return args.bundle_dir if args.bundle_dir is not None else root / BUNDLE_FOLDER


def check_bundle_arguments(args: argparse.Namespace) -> None:
    """Refuse a bad development bundle folder before the build starts."""
    root = jfg_root()
    bundle = bundle_location(args, root)
    problem = bundle and bundle_directory_problem(bundle, root)
    if problem:
        raise ValueError(problem)


def refresh_after_build(args: argparse.Namespace, executable: Path, checkout: Path,
                        find_visual_studio: Callable[[], Path]) -> None:
    """Refresh the development bundle with a freshly built runtime, when --bundle is given."""
    root = jfg_root()
    bundle = bundle_location(args, root)
    if bundle is None:
        return
    launcher = args.launcher.resolve() if args.launcher else root / LAUNCHER_FOLDER / LAUNCHER_NAME
    vc_runtime = args.vc_runtime.resolve() if args.vc_runtime else vc_runtime_folder(find_visual_studio())
    sources = BundleSources(executable=executable, vc_runtime=vc_runtime, launcher=launcher, checkout=checkout)
    LOG.debug("Bundle sources: %s", sources)
    result = refresh_dev_bundle(bundle, root, sources)
    print("Development bundle: " + str(result.bundle), flush=True)
    if not result.copied:
        print("  up to date", flush=True)
    print("Start the launcher to test: " + str(result.bundle / LAUNCHER_NAME), flush=True)
    print("Never publish the development bundle. It contains code compiled from your ROM.", flush=True)
