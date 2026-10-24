#!/usr/bin/env python3
"""Incremental development build from the ROM in a stable workspace per checkout.

Fork-only. Unlike build_from_rom.py, every run reuses one workspace,
%LOCALAPPDATA%\\JFG\\b\\<checkout-hash>\\dev, and runs only the steps that are
out of date: generate when the generated-code store (%LOCALAPPDATA%\\JFG\\g) has
no valid entry for the ROM and generator inputs, configure when the configure
arguments changed, and always cmake --build. All generated files stay local.
"""
from __future__ import annotations

import argparse
from collections.abc import Iterable, Iterator, Mapping
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
import logging
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_paths import check_binary_path, windows_cache  # noqa: E402
from build_from_rom import NAMES, ROM_SHA1, find_cmake, validate_rom, wsl_path  # noqa: E402
from runtime_identity import write_identity  # noqa: E402
import dev_bundle  # noqa: E402
from generated_code_store import (  # noqa: E402
    KEY_LENGTH, StoreOutcome, StoreResult, ensure_store_entry, entry_problem, finish_staged_entry, read_json,
    remove_stale_leftovers, remove_tree, store_entry_path, store_key,
)
from local_app_data import jfg_root, local_app_data  # noqa: E402
from sccache_launcher import (  # noqa: E402
    CompilerCache, choose_launcher, launcher_change, launcher_configure_arguments, sccache_environment,
)

ROOT = Path(__file__).resolve().parents[1]
DEV_WORKSPACE_NAME = "dev"
GENERATED_CODE_STORE_NAME = "g"
NATIVE_BUILD_NAME = "n"
LOG_FOLDER_NAME = "logs"
STATE_FILE_NAME = "dev-build.json"
BUILD_TARGET = "jfg-native-boot"
MAX_JOBS = 32
DEFAULT_JOBS = min(os.cpu_count() or 4, MAX_JOBS)  # one per logical core
DEV_GENERATOR = "Ninja"
DEV_CACHE_FILE = ROOT / "cmake" / "dev_build.cmake"
DEFAULT_GENERATION_TIMEOUT_SECONDS = 3600
MAX_GENERATION_TIMEOUT_SECONDS = 7200


def dev_workspace(checkout: Path, cache_base: Path | None = None) -> Path:
    """Return the development workspace of a checkout; the same path on every run."""
    workspace = windows_cache(checkout, cache_base) / DEV_WORKSPACE_NAME
    check_binary_path(native_build_tree(workspace))
    return workspace


def generated_code_store_root(jfg_base: Path | None = None) -> Path:
    """Return the generated-code store shared by every checkout: %LOCALAPPDATA%\\JFG\\g."""
    if jfg_base is None:
        jfg_base = jfg_root()
    store = jfg_base.resolve() / GENERATED_CODE_STORE_NAME
    # Generated sources compile straight from the entry, so its path counts toward MSBuild's limit.
    check_binary_path(store_entry_path(store, "0" * KEY_LENGTH) / "normalized")
    return store


def native_build_tree(workspace: Path) -> Path:
    """Return the CMake build tree inside a development workspace."""
    return workspace / NATIVE_BUILD_NAME


# Generator inputs (see the glossary): everything besides the ROM that decides
# the generated code. Keep this list in step with generate_from_rom.py: the
# scripts it runs or imports (transitively), the files it copies into its
# output, and the pinned dependency revisions.
GENERATOR_INPUT_FILES = (
    "dependencies.lock.json",
    "include/jfg/boot/original_timing_fast.h",
    "scripts/analyze_phase3_overlays.py",
    "scripts/apply_n64recomp_patchset.py",
    "scripts/build_private_generated_root.py",
    "scripts/check_repository_hygiene.py",
    "scripts/generate_from_rom.py",
    "scripts/identify_libultra.py",
    "scripts/native_mmio_access.py",
    "scripts/native_timing_spans.py",
    "scripts/original_timing_generation.py",
    "scripts/phase9_recover_task_helper.py",
    "scripts/prepare_n64recomp_context_dump.py",
    "scripts/probe_n64recomp_cpu.py",
    "scripts/public_safe.py",
    "scripts/transform_private_n64recomp_inputs.py",
    "scripts/validate_elf.py",
    "src/bootstrap/brokered-rsp.hpp",
    "src/bootstrap/production-audio-adapter.cpp",
)
# Every file under these folders is an input: the N64Recomp patch set.
GENERATOR_INPUT_FOLDERS = ("patches/n64recomp",)


def generator_input_paths(root: Path) -> list[str]:
    """Return the generator inputs of a checkout as sorted repo-relative POSIX paths."""
    paths = set(GENERATOR_INPUT_FILES)
    for folder in GENERATOR_INPUT_FOLDERS:
        for path in (root / folder).rglob("*"):
            if path.is_file():
                paths.add(path.relative_to(root).as_posix())
    return sorted(paths)


def generator_input_fingerprint(root: Path, relative_paths: Iterable[str]) -> dict[str, str]:
    """Return the SHA-256 of each generator input's bytes, keyed by repo-relative POSIX path.

    The keys never contain absolute paths, so equal inputs give an equal
    fingerprint in every checkout.
    """
    fingerprint = {}
    for relative in sorted(relative_paths):
        path = root / relative
        if not path.is_file():
            raise ValueError("Generator input is missing: " + relative)
        fingerprint[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return fingerprint


@dataclass(frozen=True)
class WorkspaceState:
    """What a development workspace holds before a run; the empty default is a new workspace."""

    # Key of the store entry the build tree was last configured against.
    store_key: str | None = None
    configure_arguments: list[str] | None = None
    configure_succeeded: bool = False
    has_cmake_cache: bool = False
    # The generator named in the CMake cache, or None when unknown.
    cmake_generator: str | None = None


@dataclass(frozen=True)
class BuildPlan:
    """Whether a run configures before it builds, and why."""

    configure: bool
    reasons: tuple[str, ...]
    build: bool = True
    # Delete the build tree before configuring: CMake can't switch generators in place.
    fresh_build_tree: bool = False


def generator_mismatch(state: WorkspaceState) -> str | None:
    """Return why the build tree must start fresh because another generator configured it."""
    if not state.has_cmake_cache or state.cmake_generator in (None, DEV_GENERATOR):
        return None
    return f"build tree was configured with {state.cmake_generator}, not {DEV_GENERATOR}: starting it fresh"


def configure_problem(state: WorkspaceState, configure_arguments: list[str]) -> str | None:
    """Return why the build tree must be configured, or None when cmake --build suffices."""
    mismatch = generator_mismatch(state)
    if mismatch:
        return mismatch
    if not state.has_cmake_cache:
        return "no CMake cache"
    if not state.configure_succeeded:
        return "the last configure did not succeed"
    if state.configure_arguments != configure_arguments:
        return launcher_change(state.configure_arguments, configure_arguments) or "configure arguments changed"
    return None


def plan_build(state: WorkspaceState, entry_key: str, configure_arguments: list[str], *,
               reconfigure: bool = False, clean: bool = False, regenerated: bool = False) -> BuildPlan:
    """Decide whether a run configures before it builds."""
    if clean:
        return BuildPlan(configure=True, reasons=("--clean given",))
    mismatch = generator_mismatch(state)
    if regenerated:
        configure_reason = "configure follows a regeneration"
    elif reconfigure:
        configure_reason = "--reconfigure given"
    else:
        configure_reason = configure_problem(state, configure_arguments)
    if not configure_reason:
        return BuildPlan(configure=False, reasons=())
    reasons = [configure_reason]
    if mismatch and mismatch != configure_reason:
        reasons.append(mismatch)
    if state.store_key and state.store_key != entry_key:
        reasons.append(f"generated-code store key changed: {state.store_key} -> {entry_key}")
    return BuildPlan(configure=True, reasons=tuple(reasons), fresh_build_tree=mismatch is not None)


@dataclass(frozen=True)
class GeneratedCode:
    """Where the generated code a build compiles lives."""

    normalized: Path
    libultra: Path
    audio: Path


def generated_code_location(entry: Path) -> GeneratedCode:
    """Return the generated code of a store entry, in generate_from_rom.py's layout."""
    return GeneratedCode(normalized=entry / "normalized", libultra=entry / "libultra.json", audio=entry / "audio")


def generated_code_configure_arguments(generated: GeneratedCode) -> list[str]:
    """Return the configure arguments that point CMake at the generated code."""
    return ["-DJFG_GENERATED_ROOT=" + str(generated.normalized),
            "-DJFG_PHASE6_LIBULTRA_IDENTIFICATION=" + str(generated.libultra),
            "-DJFG_PHASE8_PRIVATE_AUDIO_ADAPTER_ROOT=" + str(generated.audio)]


def configure_arguments(build: Path, generated: GeneratedCode, dependencies: Path, ninja: Path,
                        launcher: Path | None = None) -> list[str]:
    """Return the cmake configure arguments (without the cmake executable).

    The -D definitions must match build_from_rom.py's configure step, so a
    development build compiles the same runtime as a from-ROM build. Two are
    left out: JFG_SUPPORT_SYMBOLS (cmake/dev_build.cmake switches it off for
    /Z7 and links with /DEBUG itself) and JFG_MSVC_GENERATED_COMPILE_JOBS
    (MSBuild-only; Ninja parallelises across files). The compiler launcher
    (sccache, or None) is development-build only.
    """
    return ["-S", str(ROOT), "-B", str(build), "-C", str(DEV_CACHE_FILE),
            "-G", DEV_GENERATOR, "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_MAKE_PROGRAM=" + str(ninja),
            "-DBUILD_TESTING=OFF", "-DJFG_ENABLE_GENERATED_CODE=ON", "-DJFG_BUILD_PHASE6_NATIVE_BOOT=ON",
            "-DJFG_BUILD_PHASE8_LIVE=ON", "-DJFG_ENABLE_RT64=ON",
            *generated_code_configure_arguments(generated),
            "-DJFG_PHASE8_PRIVATE_AUDIO_RUNTIME_INCLUDE=" + str(dependencies / "N64ModernRuntime/librecomp/include"),
            "-DJFG_RT64_ROOT=" + str(dependencies / "rt64"),
            *launcher_configure_arguments(launcher)]


def runtime_executable(build: Path) -> Path:
    """Return the runtime executable that building the build tree produces.

    Ninja is single-config, so the exe lands in the build tree root, not in
    Release as with the Visual Studio generator.
    """
    return build / (BUILD_TARGET + ".exe")


def build_command(cmake: str, build: Path, jobs: int) -> list[str]:
    """Return the cmake --build command; the job count is passed only here."""
    return [cmake, "--build", str(build), "--target", BUILD_TARGET, "--parallel", str(jobs)]


CMAKE_CACHE_GENERATOR = re.compile(r"^CMAKE_GENERATOR:INTERNAL=(.*)$", re.M)


def cache_generator(cmake_cache: str) -> str | None:
    """Return the generator a CMakeCache.txt names, or None when it names none."""
    match = CMAKE_CACHE_GENERATOR.search(cmake_cache)
    return match.group(1).strip() if match else None


@dataclass(frozen=True)
class VisualStudioTools:
    """The MSVC environment script and Ninja of a Visual Studio 2022 install."""

    vcvars: Path
    ninja: Path


VCVARS_PATH = "VC/Auxiliary/Build/vcvars64.bat"
NINJA_PATH = "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"


def visual_studio_tools(install: Path) -> VisualStudioTools:
    """Return vcvars64.bat and the bundled ninja.exe of an install; fail naming a missing one."""
    tools = VisualStudioTools(vcvars=install / VCVARS_PATH, ninja=install / NINJA_PATH)
    for tool in (tools.vcvars, tools.ninja):
        if not tool.is_file():
            raise ValueError(f"Required tool missing: {tool}. Install the Visual Studio 2022 C++ desktop "
                             "tools with CMake support.")
    return tools


# Printed between vcvars64.bat's banner and the `set` listing of the environment it leaves.
ENVIRONMENT_MARKER = "===ENVIRONMENT==="


def parse_environment_listing(listing: str) -> dict[str, str]:
    """Return the variables of a `set` listing that follows ENVIRONMENT_MARKER."""
    _, marker, variables = listing.partition(ENVIRONMENT_MARKER)
    if not marker:
        raise ValueError("vcvars64.bat did not finish; the MSVC environment is unknown.")
    environment = {}
    for line in variables.splitlines():
        name, separator, value = line.partition("=")
        # cmd keeps per-drive working folders as hidden "=C:" variables; skip them.
        if separator and name:
            environment[name] = value
    return environment


def find_visual_studio() -> Path:
    """Return the VS 2022 install with C++ tools that vswhere reports, as find_cmake searches it."""
    base = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
    vswhere = base / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        raise ValueError(f"Required tool missing: {vswhere}")
    found = subprocess.check_output([str(vswhere), "-latest", "-products", "*",
                                     "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                     "-property", "installationPath"], text=True).strip().splitlines()
    if not found:
        raise ValueError("Install Visual Studio 2022 C++ desktop tools with CMake support.")
    return Path(found[0])


def msvc_environment(vcvars: Path) -> dict[str, str]:
    """Run vcvars64.bat and return the x64 MSVC environment it leaves."""
    # cmd /s strips the outer quotes and keeps the quoted path with spaces intact.
    command = f'cmd /d /s /c ""{vcvars}" >nul && echo {ENVIRONMENT_MARKER} && set"'
    listing = subprocess.run(command, capture_output=True, text=True, errors="replace", check=False).stdout
    return parse_environment_listing(listing)


def read_workspace_state(workspace: Path) -> WorkspaceState:
    """Read what the workspace holds: dev state and CMake cache."""
    dev_state = read_json(workspace / STATE_FILE_NAME) or {}
    configure = dev_state.get("configure") or {}
    cmake_cache = native_build_tree(workspace) / "CMakeCache.txt"
    has_cmake_cache = cmake_cache.is_file()
    return WorkspaceState(
        store_key=dev_state.get("store_key"),
        configure_arguments=configure.get("arguments"),
        configure_succeeded=configure.get("succeeded") is True,
        has_cmake_cache=has_cmake_cache,
        cmake_generator=(cache_generator(cmake_cache.read_text(encoding="utf-8", errors="replace"))
                         if has_cmake_cache else None),
    )


class DevState:
    """The fork-owned state file of a development workspace (dev-build.json).

    It records the store key the build tree was configured against, the last
    configure arguments and whether that configure succeeded, and the step
    timings of the last run.
    """

    def __init__(self, workspace: Path) -> None:
        self.path = workspace / STATE_FILE_NAME
        self.data = read_json(self.path) or {}

    def update(self, **values: object) -> None:
        """Merge the given top-level values into the state and write the file."""
        self.data.update(values)
        self.path.write_text(json.dumps(self.data, indent=2) + "\n", encoding="utf-8")


def remove_dev_workspace(workspace: Path, cache: Path) -> None:
    """Delete the development workspace, after checking it is this checkout's."""
    workspace = workspace.resolve()
    if workspace.parent != cache.resolve() or workspace.name != DEV_WORKSPACE_NAME:
        raise ValueError(f"Refusing to delete {workspace}: not this checkout's development workspace.")
    remove_tree(workspace)


COMPILER_ERROR = re.compile(r"\b(?:error|fatal error) (?:C[0-9]{4}|LNK[0-9]{4}|MSB[0-9]{4})\b", re.I)
# generate_from_rom.py prints each of its stage names on a line of its own.
GENERATOR_STAGE = re.compile(r"[a-z][a-z-]+\n?")


class StepRunner:
    """Runs and times the steps of a run, with one log file per command."""

    def __init__(self, workspace: Path) -> None:
        self.logs = workspace / LOG_FOLDER_NAME
        self.timings: list[dict[str, object]] = []

    @contextmanager
    def step(self, name: str) -> Iterator[None]:
        """Print a step's start, result and duration, and record its timing."""
        print(f"{name}: started", flush=True)
        started = time.monotonic()
        succeeded = False
        try:
            yield
            succeeded = True
        finally:
            seconds = round(time.monotonic() - started, 1)
            self.timings.append({"step": name, "seconds": seconds, "succeeded": succeeded})
            print(f"{name}: {'done' if succeeded else 'failed'} in {seconds} s", flush=True)

    def run(self, name: str, command: list[str], cwd: Path = ROOT, env: Mapping[str, str] | None = None) -> None:
        """Run a command with its output in the log; raise naming the log on failure."""
        self.logs.mkdir(parents=True, exist_ok=True)
        log = self.logs / (name + ".log")
        with log.open("w", encoding="utf-8", buffering=1) as stream:
            with subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, errors="replace", env=env) as process:
                for line in process.stdout:
                    stream.write(line)
                    is_progress = name == "generate" and GENERATOR_STAGE.fullmatch(line)
                    if is_progress or COMPILER_ERROR.search(line):
                        print("  " + line.strip(), flush=True)
                exit_code = process.wait()
        if exit_code:
            raise RuntimeError(f"{name} failed (exit code {exit_code}). Local diagnostic log: {log}")


def prepare_dependencies(runner: StepRunner, dependencies: Path) -> None:
    """Clone the pinned dependencies that are missing; check the pinned revision of all."""
    # The public lock file supplies repository URLs and immutable revisions.
    lock = json.loads((ROOT / "dependencies.lock.json").read_text())
    for entry in lock["repositories"]:
        if entry["id"] not in NAMES:
            continue
        destination = dependencies / NAMES[entry["id"]]
        if not destination.exists():
            print(f"  {entry['id']} is missing: cloning it", flush=True)
            destination.parent.mkdir(parents=True, exist_ok=True)
            runner.run("clone-" + entry["id"], ["git", "clone", "--config", "core.autocrlf=false",
                       "--config", "core.longpaths=true", "--no-checkout", entry["url"], str(destination)])
            runner.run("pin-" + entry["id"], ["git", "-C", str(destination), "checkout", "--detach", entry["commit"]])
            runner.run("submodules-" + entry["id"], ["git", "-c", "core.autocrlf=false", "-c", "core.longpaths=true",
                       "-C", str(destination), "submodule", "update", "--init", "--recursive"])
        head = subprocess.check_output(["git", "-C", str(destination), "rev-parse", "HEAD"], text=True).strip()
        if head != entry["commit"]:
            raise ValueError(f"Dependency revision mismatch: {entry['id']}; existing files were preserved.")


@dataclass(frozen=True)
class GenerateSettings:
    """What the generate step needs besides the workspace."""

    rom: Path
    dependencies: Path
    distro: str
    timeout_seconds: int
    jobs: int


def run_generator(runner: StepRunner, workspace: Path, settings: GenerateSettings) -> None:
    """Generate the code from the ROM into an empty folder with the command build_from_rom.py uses."""
    runner.run("generate", ["wsl", "-d", settings.distro, "--exec", "timeout", "--kill-after=3s",
               str(settings.timeout_seconds) + "s", "python3",
               wsl_path(ROOT / "scripts/generate_from_rom.py"), "--rom", wsl_path(settings.rom),
               "--workspace", wsl_path(workspace), "--dependency-root", wsl_path(settings.dependencies),
               "--jobs", str(settings.jobs)])


STORE_OUTCOME_MESSAGES = {
    StoreOutcome.REUSED: "generate: skipped (reused store entry {key})",
    StoreOutcome.LOST_RACE: "generate: store entry {key} was published by another run meanwhile; using it",
    StoreOutcome.GENERATED: "generate: published store entry {key}",
    StoreOutcome.REPLACED: "generate: replaced store entry {key} ({reason})",
}
# Outcomes where this run published a new entry, so the build tree must be configured against it.
REGENERATED_OUTCOMES = frozenset({StoreOutcome.GENERATED, StoreOutcome.REPLACED})


def get_store_entry(runner: StepRunner, store: Path, generator_inputs: dict[str, str],
                    settings: GenerateSettings, *, regenerate: bool) -> StoreResult:
    """Find the store entry for the ROM and generator inputs, generating it when it is missing or unusable."""
    key = store_key(ROM_SHA1, generator_inputs)

    def generate_entry(staging: Path) -> None:
        with runner.step("generate"):
            run_generator(runner, staging, settings)
            finish_staged_entry(staging, key, ROM_SHA1, generator_inputs)

    result = ensure_store_entry(store, key, generate_entry, lambda entry: entry_problem(entry, key, ROM_SHA1),
                                regenerate=regenerate)
    print(STORE_OUTCOME_MESSAGES[result.outcome].format(key=key, reason=result.reason), flush=True)
    return result


def play(executable: Path, rom: Path) -> int:
    """Start the runtime exactly as build_from_rom.py --play does."""
    data_root = (Path(os.environ["JFG_DATA_ROOT"]) if os.environ.get("JFG_DATA_ROOT")
                 else local_app_data() / "JFGRecomp")
    profile = data_root / "profiles/default"
    profile.mkdir(parents=True, exist_ok=True)
    environment = {key: value for key, value in os.environ.items() if not key.startswith("JFG_")}
    return subprocess.call([str(executable), "--rom", str(rom), "--save", str(profile / "jfg.flash"),
                            "--controller-pak", str(profile / "controller-1.pak"), "--play"],
                           cwd=executable.parent, env=environment)


def argument_parser() -> argparse.ArgumentParser:
    """Return the parser for dev_build.py's command line (range checks are in parse_arguments)."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--distro", default="Ubuntu-24.04")
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS,
                        help="Parallel compile jobs (default: the number of logical cores)")
    parser.add_argument("--generation-timeout", type=int, default=DEFAULT_GENERATION_TIMEOUT_SECONDS,
                        help="Hard limit for the Linux subprocess group, in seconds")
    parser.add_argument("--play", action="store_true", help="Start the runtime after a successful build")
    parser.add_argument("--reconfigure", action="store_true", help="Configure even when nothing requires it")
    parser.add_argument("--clean", action="store_true",
                        help="Delete the development workspace and start over (keeps the generated-code store)")
    parser.add_argument("--regenerate", action="store_true",
                        help="Move the generated-code store entry aside and generate it again")
    parser.add_argument("--no-sccache", action="store_true",
                        help="Compile without sccache even when it is installed (e.g. for baseline timings)")
    dev_bundle.add_bundle_arguments(parser)
    return parser


def parse_arguments() -> argparse.Namespace:
    parser = argument_parser()
    args = parser.parse_args()
    if os.name != "nt" or sys.version_info < (3, 11):
        parser.error("This build requires Windows x64 and Python 3.11 or newer.")
    if not 1 <= args.jobs <= MAX_JOBS:
        parser.error(f"--jobs must be between 1 and {MAX_JOBS}")
    if not 1 <= args.generation_timeout <= MAX_GENERATION_TIMEOUT_SECONDS:
        parser.error(f"--generation-timeout must be between 1 and {MAX_GENERATION_TIMEOUT_SECONDS}")
    return args


# Steps a run leaves out when nothing requires them; the summary names the ones skipped.
SKIPPABLE_STEPS = ("generate", "configure")


def summary_line(timings: Iterable[Mapping[str, object]], total_seconds: float, *, sccache_used: bool) -> str:
    """Return the line that ends a run: total time, the steps run and skipped, and whether sccache compiled."""
    ran = [str(timing["step"]) for timing in timings]
    skipped = [name for name in SKIPPABLE_STEPS if name not in ran]
    skipped_text = "; skipped " + ", ".join(skipped) if skipped else ""
    sccache_text = "; sccache used" if sccache_used else "; sccache not used"
    return f"Development build done in {total_seconds} s: ran {', '.join(ran)}{skipped_text}{sccache_text}"


def main() -> int:
    args = parse_arguments()
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    started = time.monotonic()
    rom = args.rom.resolve()
    validate_rom(rom)  # Reject before downloads or build output.
    dev_bundle.check_bundle_arguments(args)
    cmake = find_cmake()
    tools = visual_studio_tools(find_visual_studio())
    for tool in ("git", "wsl"):
        if not shutil.which(tool):
            raise ValueError(f"Required tool missing: {tool}")
    cache = windows_cache(ROOT)
    dependencies = (cache / "d").resolve()
    workspace = dev_workspace(ROOT)
    build = native_build_tree(workspace)
    print("Development workspace: " + str(workspace), flush=True)
    if args.clean:
        print("--clean given: deleting the development workspace", flush=True)
        remove_dev_workspace(workspace, cache)
    workspace.mkdir(parents=True, exist_ok=True)
    store = generated_code_store_root()
    remove_stale_leftovers(store)

    generator_inputs = generator_input_fingerprint(ROOT, generator_input_paths(ROOT))
    runner = StepRunner(workspace)
    state = DevState(workspace)
    # Ninja, unlike MSBuild, needs cl and link on PATH; captured once per run.
    msvc = msvc_environment(tools.vcvars)
    launcher = choose_launcher(no_sccache=args.no_sccache, found=shutil.which("sccache"))
    logging.log(launcher.level, launcher.message)
    # Set even without a launcher, so nothing ever fills sccache's default cache folder.
    msvc.update(sccache_environment(local_app_data()))
    with runner.step("dependencies"):
        prepare_dependencies(runner, dependencies)
    entry = get_store_entry(runner, store, generator_inputs, GenerateSettings(
        rom, dependencies, args.distro, args.generation_timeout, args.jobs), regenerate=args.regenerate)
    print("Generated code: " + str(entry.path), flush=True)

    entry_key = entry.path.name
    arguments = configure_arguments(build, generated_code_location(entry.path), dependencies, tools.ninja,
                                    launcher.launcher)
    plan = plan_build(read_workspace_state(workspace), entry_key, arguments, reconfigure=args.reconfigure,
                      clean=args.clean, regenerated=entry.outcome in REGENERATED_OUTCOMES)
    for reason in plan.reasons:
        print("Reason: " + reason, flush=True)
    if plan.configure:
        state.update(configure={"arguments": arguments, "succeeded": False})
        if plan.fresh_build_tree:
            print("configure: deleting the build tree " + str(build), flush=True)
            remove_tree(build)
        with runner.step("configure"):
            runner.run("configure", [cmake, *arguments], env=msvc)
        state.update(store_key=entry_key, configure={"arguments": arguments, "succeeded": True})
    else:
        print("configure: skipped (configure arguments unchanged)", flush=True)
    compiler_cache = CompilerCache(launcher.launcher, msvc) if launcher.launcher else None
    if compiler_cache:
        compiler_cache.start()
    try:
        with runner.step("build"):
            runner.run("build", build_command(cmake, build, args.jobs), env=msvc)
    finally:
        if compiler_cache:
            compiler_cache.report()
    executable = runtime_executable(build)
    if not executable.is_file():
        raise RuntimeError("Build finished without the native executable.")
    write_identity(executable, ROOT)
    if args.bundle:
        with runner.step("bundle"):
            dev_bundle.refresh_after_build(args, executable, ROOT, find_visual_studio)

    total_seconds = round(time.monotonic() - started, 1)
    state.update(last_run={"steps": runner.timings, "total_seconds": total_seconds})
    print(summary_line(runner.timings, total_seconds, sccache_used=compiler_cache is not None), flush=True)
    print("Built: " + str(executable), flush=True)
    print("Keep this build local. It contains code generated from your ROM.", flush=True)
    if args.play:
        return play(executable, rom)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
