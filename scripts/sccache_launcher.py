"""sccache as the compiler launcher of fork development builds (scripts/dev_build.py).

Fork-only. When sccache is on PATH, C/C++ compiles go through it, with its
cache in %LOCALAPPDATA%\\JFG\\cache\\sccache, so clean builds and other
checkouts reuse objects compiled from the same input.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import logging
from pathlib import Path
import subprocess
import time

LOG = logging.getLogger("sccache_launcher")

INSTALL_COMMAND = "winget install Mozilla.sccache"


@dataclass(frozen=True)
class LauncherDecision:
    """The compiler launcher a build uses (None: compile directly) and the line that says so."""

    launcher: Path | None
    message: str
    level: int


def choose_launcher(*, no_sccache: bool, found: str | None) -> LauncherDecision:
    """Decide on the compiler launcher from --no-sccache and where sccache was found on PATH."""
    if no_sccache:
        return LauncherDecision(None, "sccache: not used (--no-sccache given)", logging.INFO)
    if found is None:
        return LauncherDecision(None, "sccache is not on PATH: building without a compiler cache. "
                                f"Install it with `{INSTALL_COMMAND}` for faster clean builds.", logging.WARNING)
    launcher = Path(found)
    return LauncherDecision(launcher, f"sccache: compiling through {launcher}", logging.INFO)


LAUNCHER_VARIABLES = ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER")


def launcher_configure_arguments(launcher: Path | None) -> list[str]:
    """Return the -D arguments that set the C and C++ compiler launcher.

    Without a launcher both are set empty rather than left out: the CMake cache
    would otherwise keep a launcher an earlier configure set.
    """
    value = str(launcher) if launcher else ""
    return [f"-D{variable}={value}" for variable in LAUNCHER_VARIABLES]


def recorded_launcher(configure_arguments: list[str] | None) -> str | None:
    """Return the C compiler launcher in configure arguments, or None when they set none."""
    prefix = f"-D{LAUNCHER_VARIABLES[0]}="
    for argument in configure_arguments or ():
        if argument.startswith(prefix):
            return argument[len(prefix):] or None
    return None


def launcher_change(recorded: list[str] | None, wanted: list[str]) -> str | None:
    """Return why configuring is needed when the compiler launcher changed, or None when it didn't."""
    before, after = recorded_launcher(recorded), recorded_launcher(wanted)
    if before == after:
        return None
    return f"compiler launcher changed: {before or 'none'} -> {after or 'none'}"


CACHE_FOLDER = Path("JFG") / "cache" / "sccache"
CACHE_SIZE = "20G"
# sccache reads size suffixes as binary multiples, so 20G is 20 GiB.
CACHE_SIZE_BYTES = 20 * 1024 ** 3


def sccache_environment(local_app_data: Path) -> dict[str, str]:
    """Return the variables that put sccache's cache under %LOCALAPPDATA%\JFG with its size cap."""
    return {"SCCACHE_DIR": str(local_app_data / CACHE_FOLDER), "SCCACHE_CACHE_SIZE": CACHE_SIZE}


@dataclass(frozen=True)
class CompileStats:
    """Compile counts from sccache's stats: cumulative, or one build's share of them."""

    requests: int
    hits: int
    misses: int
    non_cacheable: int

    @property
    def hit_rate(self) -> float | None:
        """Share of cacheable compiles served from the cache, or None when there were none."""
        cacheable = self.hits + self.misses
        return self.hits / cacheable if cacheable else None


def count(value: object) -> int:
    """Return a stats count, summing per-language counts ({"counts": {...}}); 0 when absent or unreadable."""
    if isinstance(value, dict):
        value = sum(count(item) for item in (value.get("counts") or {}).values())
    return value if isinstance(value, int) else 0


def compile_stats(document: dict) -> CompileStats:
    """Read the compile counts of an `sccache --show-stats --stats-format=json` document."""
    stats = document.get("stats") or {}
    return CompileStats(
        requests=count(stats.get("compile_requests")),
        hits=count(stats.get("cache_hits")),
        misses=count(stats.get("cache_misses")),
        non_cacheable=count(stats.get("requests_not_cacheable")) + count(stats.get("non_cacheable_compilations")),
    )


def stats_delta(before: dict, after: dict) -> CompileStats:
    """Return the compile counts between two stats documents of the same server."""
    start, end = compile_stats(before), compile_stats(after)
    # Counts are cumulative since the server started; fewer requests after means it restarted meanwhile.
    if end.requests < start.requests:
        return end
    return CompileStats(requests=end.requests - start.requests, hits=end.hits - start.hits,
                        misses=end.misses - start.misses, non_cacheable=end.non_cacheable - start.non_cacheable)


def describe_build_stats(stats: CompileStats) -> str:
    """Return the line that reports a build's sccache counts."""
    rate = "n/a" if stats.hit_rate is None else f"{stats.hit_rate:.1%}"
    return (f"sccache: {stats.requests} compile requests, {stats.hits} hits, {stats.misses} misses, "
            f"{stats.non_cacheable} non-cacheable; hit rate {rate}")


def comparable_path(text: str) -> str:
    # sccache prints the local cache as `Local disk: "C:\Users\..."` (Rust debug format, backslashes doubled).
    return text.replace("/", "\\").replace("\\\\", "\\").lower()


def server_settings_problem(document: dict, environment: dict[str, str]) -> str | None:
    """Return why the running sccache server doesn't use our cache folder or cap, or None when it does.

    The server reads SCCACHE_DIR and SCCACHE_CACHE_SIZE only when it starts, so
    a server another shell started keeps its own settings.
    """
    wanted_folder = environment["SCCACHE_DIR"]
    location = str(document.get("cache_location", ""))
    max_size = document.get("max_cache_size")
    problems = []
    if comparable_path(wanted_folder) not in comparable_path(location):
        problems.append(f"cache location {location or 'unknown'} instead of {wanted_folder}")
    if max_size != CACHE_SIZE_BYTES:
        problems.append(f"max size {max_size} bytes instead of {CACHE_SIZE_BYTES}")
    if not problems:
        return None
    return ("Another sccache server is running with other settings (" + "; ".join(problems) + "). "
            "Run `sccache --stop-server` when no build is running, then build again.")


SCCACHE_TIMEOUT_SECONDS = 60


class CompilerCache:
    """The sccache server a build compiles through: started before the build, its stats read around it."""

    def __init__(self, sccache: Path, environment: dict[str, str]) -> None:
        self.sccache = sccache
        self.environment = environment
        self.before: dict | None = None

    def start(self) -> None:
        """Start the server with our cache settings, warn when a running one has others, and take stats."""
        started = time.monotonic()
        # The server outlives this run (other worktrees share it), so it must not hold our pipes open.
        result = subprocess.run([str(self.sccache), "--start-server"], env=self.environment,
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                timeout=SCCACHE_TIMEOUT_SECONDS, check=False)
        # A non-zero exit usually means a server is already running, which is fine.
        LOG.debug("sccache --start-server exited with %s after %.1f s", result.returncode,
                  time.monotonic() - started)
        self.before = self.read_stats()
        if self.before is None:
            return
        problem = server_settings_problem(self.before, self.environment)
        if problem:
            LOG.warning(problem)

    def report(self) -> None:
        """Print this build's compile counts and hit rate; a warning only when stats can't be read."""
        after = self.read_stats()
        if self.before is None or after is None:
            return
        print(describe_build_stats(stats_delta(self.before, after)), flush=True)

    def read_stats(self) -> dict | None:
        """Return the server's stats document, or None (with a warning) when it can't be read."""
        try:
            output = subprocess.run([str(self.sccache), "--show-stats", "--stats-format=json"],
                                    env=self.environment, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, timeout=SCCACHE_TIMEOUT_SECONDS, check=True).stdout
            document = json.loads(output)
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            LOG.warning("sccache stats are unavailable, so this build's hit rate is unknown: %s", error)
            return None
        if not isinstance(document, dict):
            LOG.warning("sccache stats are not a JSON object, so this build's hit rate is unknown.")
            return None
        return document
