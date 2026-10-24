#!/usr/bin/env python3
"""Incremental development build from the ROM in a stable workspace per checkout.

Fork-only. Unlike build_from_rom.py, every run reuses one workspace,
%LOCALAPPDATA%\\JFG\\b\\<checkout-hash>\\dev, and runs only the steps that are
out of date: generate when the generated code no longer matches the ROM or the
generator inputs, configure when the configure arguments changed, and always
cmake --build. All generated files stay local.
"""
from __future__ import annotations

import argparse
from collections.abc import Iterable, Iterator, Mapping
from contextlib import contextmanager
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_paths import check_binary_path, windows_cache  # noqa: E402
from build_from_rom import NAMES, ROM_SHA1, find_cmake, validate_rom, wsl_path  # noqa: E402
from runtime_identity import write_identity  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEV_WORKSPACE_NAME = "dev"
NATIVE_BUILD_NAME = "n"
LOG_FOLDER_NAME = "logs"
STATE_FILE_NAME = "dev-build.json"
BUILD_TARGET = "jfg-native-boot"
DEFAULT_JOBS = min(os.cpu_count() or 4, 8)  # build_from_rom.py's default


def dev_workspace(checkout: Path, cache_base: Path | None = None) -> Path:
    """Return the development workspace of a checkout; the same path on every run."""
    workspace = windows_cache(checkout, cache_base) / DEV_WORKSPACE_NAME
    check_binary_path(native_build_tree(workspace))
    return workspace


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


def changed_generator_inputs(recorded: dict[str, str], current: dict[str, str]) -> list[str]:
    """Return the sorted paths whose digest differs, including added and removed inputs."""
    names = set(recorded) | set(current)
    return sorted(name for name in names if recorded.get(name) != current.get(name))


# The files generate_from_rom.py lists in generation.json; the same set that
# build_from_rom.py --resume-native requires.
GENERATED_FILES = frozenset({
    "normalized/sources.json", "libultra.json", "audio/production-audio-adapter.cpp",
    "audio/brokered-rsp.hpp", "audio/brokered-jfg-audio-probe.cpp", "audio/jfg-audio-probe.cpp",
    "normalized/jfg_original_timing.h", "normalized/original-timing.json",
    "normalized/jfg_original_timing_fast.h", "normalized/funcs.h",
})


@dataclass(frozen=True)
class WorkspaceState:
    """What a development workspace holds before a run; the empty default is a new workspace."""

    # Contents of generation.json, or None when it is missing or unreadable.
    generation: Mapping[str, object] | None = None
    # SHA-256 of each file generation.json lists, as found on disk (None: missing).
    generated_file_digests: Mapping[str, str | None] = field(default_factory=dict)
    # Generator-input fingerprint recorded when the generated code was made.
    generator_inputs: Mapping[str, str] | None = None
    configure_arguments: list[str] | None = None
    configure_succeeded: bool = False
    has_cmake_cache: bool = False


@dataclass(frozen=True)
class BuildPlan:
    """The steps a run performs, and why it generates or configures."""

    generate: bool
    configure: bool
    reasons: tuple[str, ...]
    build: bool = True


def generation_problem(state: WorkspaceState) -> str | None:
    """Return why the generated code can't be used, with the checks of --resume-native."""
    generation = state.generation
    if generation is None:
        return "no generation.json"
    if generation.get("complete") is not True:
        return "generation.json is incomplete"
    if generation.get("rom_sha1") != ROM_SHA1:
        return "generated from another ROM"
    recorded_files = generation.get("files")
    if not isinstance(recorded_files, dict) or set(recorded_files) != GENERATED_FILES:
        return "generation.json lists unexpected files"
    changed = sorted(name for name, digest in recorded_files.items()
                     if state.generated_file_digests.get(name) != digest)
    if changed:
        return "generated files missing or changed: " + ", ".join(changed)
    return None


def generator_inputs_problem(state: WorkspaceState, generator_inputs: Mapping[str, str]) -> str | None:
    """Return why the generator inputs no longer match the generated code."""
    if state.generator_inputs is None:
        return "no generator-input fingerprint recorded"
    changed = changed_generator_inputs(dict(state.generator_inputs), dict(generator_inputs))
    if changed:
        return "generator inputs changed: " + ", ".join(Path(name).name for name in changed)
    return None


def configure_problem(state: WorkspaceState, configure_arguments: list[str]) -> str | None:
    """Return why the build tree must be configured, or None when cmake --build suffices."""
    if not state.has_cmake_cache:
        return "no CMake cache"
    if not state.configure_succeeded:
        return "the last configure did not succeed"
    if state.configure_arguments != configure_arguments:
        return "configure arguments changed"
    return None


def plan_build(state: WorkspaceState, generator_inputs: Mapping[str, str], configure_arguments: list[str],
               *, reconfigure: bool = False, clean: bool = False) -> BuildPlan:
    """Decide whether a run generates and configures before it builds."""
    if clean:
        return BuildPlan(generate=True, configure=True, reasons=("--clean given",))
    generate_reason = generation_problem(state) or generator_inputs_problem(state, generator_inputs)
    if generate_reason:
        return BuildPlan(generate=True, configure=True,
                         reasons=(generate_reason, "configure follows a regeneration"))
    configure_reason = "--reconfigure given" if reconfigure else configure_problem(state, configure_arguments)
    if configure_reason:
        return BuildPlan(generate=False, configure=True, reasons=(configure_reason,))
    return BuildPlan(generate=False, configure=False, reasons=())


@dataclass(frozen=True)
class GeneratedCode:
    """Where the generated code a build compiles lives."""

    normalized: Path
    libultra: Path
    audio: Path


def generated_code_location(workspace: Path) -> GeneratedCode:
    """Return the generated code of a workspace, in generate_from_rom.py's layout."""
    return GeneratedCode(normalized=workspace / "normalized", libultra=workspace / "libultra.json",
                         audio=workspace / "audio")


def generated_code_configure_arguments(generated: GeneratedCode) -> list[str]:
    """Return the configure arguments that point CMake at the generated code."""
    return ["-DJFG_GENERATED_ROOT=" + str(generated.normalized),
            "-DJFG_PHASE6_LIBULTRA_IDENTIFICATION=" + str(generated.libultra),
            "-DJFG_PHASE8_PRIVATE_AUDIO_ADAPTER_ROOT=" + str(generated.audio)]


def configure_arguments(build: Path, generated: GeneratedCode, dependencies: Path, jobs: int) -> list[str]:
    """Return the cmake configure arguments (without the cmake executable).

    These must match build_from_rom.py's configure step, so a development
    build compiles the same runtime as a from-ROM build.
    """
    return ["-S", str(ROOT), "-B", str(build),
            "-G", "Visual Studio 17 2022", "-A", "x64", "-DBUILD_TESTING=OFF", "-DJFG_SUPPORT_SYMBOLS=ON",
            "-DJFG_MSVC_GENERATED_COMPILE_JOBS=" + str(jobs),
            "-DJFG_ENABLE_GENERATED_CODE=ON", "-DJFG_BUILD_PHASE6_NATIVE_BOOT=ON",
            "-DJFG_BUILD_PHASE8_LIVE=ON", "-DJFG_ENABLE_RT64=ON",
            *generated_code_configure_arguments(generated),
            "-DJFG_PHASE8_PRIVATE_AUDIO_RUNTIME_INCLUDE=" + str(dependencies / "N64ModernRuntime/librecomp/include"),
            "-DJFG_RT64_ROOT=" + str(dependencies / "rt64")]


def runtime_executable(build: Path) -> Path:
    """Return the runtime executable that building the build tree produces."""
    return build / "Release" / (BUILD_TARGET + ".exe")


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


def read_workspace_state(workspace: Path) -> WorkspaceState:
    """Read what the workspace holds: generation.json, the generated files, dev state and CMake cache."""
    generation = read_json(workspace / "generation.json")
    recorded_files = generation.get("files") if generation else None
    digests = {}
    if isinstance(recorded_files, dict):
        # Hash only the known names, so generation.json can't point outside the workspace.
        for name in GENERATED_FILES & set(recorded_files):
            digests[name] = file_sha256(workspace / name)
    dev_state = read_json(workspace / STATE_FILE_NAME) or {}
    configure = dev_state.get("configure") or {}
    return WorkspaceState(
        generation=generation,
        generated_file_digests=digests,
        generator_inputs=dev_state.get("generator_inputs"),
        configure_arguments=configure.get("arguments"),
        configure_succeeded=configure.get("succeeded") is True,
        has_cmake_cache=(native_build_tree(workspace) / "CMakeCache.txt").is_file(),
    )


class DevState:
    """The fork-owned state file of a development workspace (dev-build.json).

    It records the generator-input fingerprint of the generated code, the last
    configure arguments and whether that configure succeeded, and the step
    timings of the last run. generation.json stays upstream's generator's.
    """

    def __init__(self, workspace: Path) -> None:
        self.path = workspace / STATE_FILE_NAME
        self.data = read_json(self.path) or {}

    def update(self, **values: object) -> None:
        self.data.update(values)
        self.path.write_text(json.dumps(self.data, indent=2) + "\n", encoding="utf-8")


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


def remove_dev_workspace(workspace: Path, cache: Path) -> None:
    """Delete the development workspace, after checking it is this checkout's."""
    workspace = workspace.resolve()
    if workspace.parent != cache.resolve() or workspace.name != DEV_WORKSPACE_NAME:
        raise ValueError(f"Refusing to delete {workspace}: not this checkout's development workspace.")
    remove_tree(workspace)


def remove_generated_outputs(workspace: Path) -> None:
    """Delete generation.json and everything generation wrote; keep the build tree, logs and state.

    generate_from_rom.py refuses to write into some existing folders
    (context, helper-*), and without generation.json an interrupted
    generation is seen as incomplete on the next run.
    """
    kept = {NATIVE_BUILD_NAME, LOG_FOLDER_NAME, STATE_FILE_NAME}
    (workspace / "generation.json").unlink(missing_ok=True)
    for item in workspace.iterdir():
        if item.name in kept:
            continue
        if item.is_dir() and not item.is_symlink():
            remove_tree(item)
        else:
            item.unlink()


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

    def run(self, name: str, command: list[str], cwd: Path = ROOT) -> None:
        """Run a command with its output in the log; raise naming the log on failure."""
        self.logs.mkdir(parents=True, exist_ok=True)
        log = self.logs / (name + ".log")
        with log.open("w", encoding="utf-8", buffering=1) as stream:
            with subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, errors="replace") as process:
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


def generate(runner: StepRunner, workspace: Path, settings: GenerateSettings) -> GeneratedCode:
    """Generate the code from the ROM into the workspace with the command build_from_rom.py uses."""
    remove_generated_outputs(workspace)
    runner.run("generate", ["wsl", "-d", settings.distro, "--exec", "timeout", "--kill-after=3s",
               str(settings.timeout_seconds) + "s", "python3",
               wsl_path(ROOT / "scripts/generate_from_rom.py"), "--rom", wsl_path(settings.rom),
               "--workspace", wsl_path(workspace), "--dependency-root", wsl_path(settings.dependencies),
               "--jobs", str(settings.jobs)])
    return generated_code_location(workspace)


def play(executable: Path, rom: Path) -> int:
    """Start the runtime exactly as build_from_rom.py --play does."""
    data_root = (Path(os.environ["JFG_DATA_ROOT"]) if os.environ.get("JFG_DATA_ROOT")
                 else Path(os.environ["LOCALAPPDATA"]) / "JFGRecomp")
    profile = data_root / "profiles/default"
    profile.mkdir(parents=True, exist_ok=True)
    environment = {key: value for key, value in os.environ.items() if not key.startswith("JFG_")}
    return subprocess.call([str(executable), "--rom", str(rom), "--save", str(profile / "jfg.flash"),
                            "--controller-pak", str(profile / "controller-1.pak"), "--play"],
                           cwd=executable.parent, env=environment)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--distro", default="Ubuntu-24.04")
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS)
    parser.add_argument("--generation-timeout", type=int, default=3600,
                        help="Hard limit for the Linux subprocess group, in seconds")
    parser.add_argument("--play", action="store_true", help="Start the runtime after a successful build")
    parser.add_argument("--reconfigure", action="store_true", help="Configure even when nothing requires it")
    parser.add_argument("--clean", action="store_true", help="Delete the development workspace and start over")
    args = parser.parse_args()
    if os.name != "nt" or sys.version_info < (3, 11):
        parser.error("This build requires Windows x64 and Python 3.11 or newer.")
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    if not 1 <= args.generation_timeout <= 7200:
        parser.error("--generation-timeout must be between 1 and 7200")
    return args


def main() -> int:
    args = parse_arguments()
    started = time.monotonic()
    rom = args.rom.resolve()
    validate_rom(rom)  # Reject before downloads or build output.
    cmake = find_cmake()
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

    generator_inputs = generator_input_fingerprint(ROOT, generator_input_paths(ROOT))
    generated = generated_code_location(workspace)
    arguments = configure_arguments(build, generated, dependencies, args.jobs)
    plan = plan_build(read_workspace_state(workspace), generator_inputs, arguments,
                      reconfigure=args.reconfigure, clean=args.clean)
    for reason in plan.reasons:
        print("Reason: " + reason, flush=True)

    runner = StepRunner(workspace)
    state = DevState(workspace)
    with runner.step("dependencies"):
        prepare_dependencies(runner, dependencies)
    if plan.generate:
        # Forget the old fingerprint first, so an interrupted generation never looks current.
        state.update(generator_inputs=None, configure={"arguments": arguments, "succeeded": False})
        with runner.step("generate"):
            generated = generate(runner, workspace, GenerateSettings(
                rom, dependencies, args.distro, args.generation_timeout, args.jobs))
        state.update(generator_inputs=generator_inputs)
    else:
        print("generate: skipped (generated code matches the ROM and generator inputs)", flush=True)
    if plan.configure:
        state.update(configure={"arguments": arguments, "succeeded": False})
        with runner.step("configure"):
            runner.run("configure", [cmake, *arguments])
        state.update(configure={"arguments": arguments, "succeeded": True})
    else:
        print("configure: skipped (configure arguments unchanged)", flush=True)
    with runner.step("build"):
        runner.run("build", [cmake, "--build", str(build), "--config", "Release",
                   "--target", BUILD_TARGET, "--parallel", str(args.jobs)])
    executable = runtime_executable(build)
    if not executable.is_file():
        raise RuntimeError("Build finished without the native executable.")
    write_identity(executable, ROOT)

    total_seconds = round(time.monotonic() - started, 1)
    state.update(last_run={"steps": runner.timings, "total_seconds": total_seconds})
    ran = [timing["step"] for timing in runner.timings]
    skipped = [name for name in ("generate", "configure") if name not in ran]
    skipped_text = "; skipped " + ", ".join(skipped) if skipped else ""
    print(f"Development build done in {total_seconds} s: ran {', '.join(ran)}{skipped_text}", flush=True)
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
