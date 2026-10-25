"""sccache as the compiler launcher of fork development builds (scripts/dev_build.py).

Fork-only. When sccache is on PATH, C/C++ compiles go through it, with its
cache in %LOCALAPPDATA%\\JFG\\cache\\sccache, so clean builds and other
checkouts reuse objects compiled from the same input.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import logging
import re
import socket
from pathlib import Path
import subprocess
import tempfile
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


def sccache_environment(local_app_data: Path, server_port: int | None = None) -> dict[str, str]:
    r"""Return the variables that put sccache's cache under %LOCALAPPDATA%\JFG with its size cap.

    With a server port, they also make sccache listen on it.
    """
    environment = {"SCCACHE_DIR": str(local_app_data / CACHE_FOLDER), "SCCACHE_CACHE_SIZE": CACHE_SIZE}
    if server_port is not None:
        environment[SERVER_PORT_VARIABLE] = str(server_port)
    return environment


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


def stats_count(value: object) -> int:
    """Return a stats count, summing per-language counts ({"counts": {...}}); 0 when absent or unreadable."""
    if isinstance(value, dict):
        value = sum(stats_count(item) for item in (value.get("counts") or {}).values())
    return value if isinstance(value, int) else 0


def compile_stats(document: dict) -> CompileStats:
    """Read the compile counts of an `sccache --show-stats --stats-format=json` document."""
    stats = document.get("stats") or {}
    return CompileStats(
        requests=stats_count(stats.get("compile_requests")),
        hits=stats_count(stats.get("cache_hits")),
        misses=stats_count(stats.get("cache_misses")),
        non_cacheable=(stats_count(stats.get("requests_not_cacheable"))
                       + stats_count(stats.get("non_cacheable_compilations"))),
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
    r"""Return a path in a form that compares equal however sccache printed it.

    sccache prints the local cache as `Local disk: "D:\\cache"` (Rust debug
    format, backslashes doubled), so slashes are unified, doubled backslashes
    collapsed and case folded.
    """
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


@dataclass(frozen=True)
class PortRange:
    """A range of TCP ports, both ends included."""

    first: int
    last: int

    def __contains__(self, port: int) -> bool:
        return self.first <= port <= self.last


# A range line of `netsh int ipv4 show excludedportrange`: start and end port, then `*` when administered.
EXCLUDED_RANGE_LINE = re.compile(r"^\s*(\d+)\s+(\d+)\s*\*?\s*$", re.MULTILINE)


def parse_excluded_port_ranges(netsh_output: str) -> list[PortRange]:
    """Return the port ranges `netsh int ipv4 show excludedportrange protocol=tcp` lists."""
    return [PortRange(int(first), int(last)) for first, last in EXCLUDED_RANGE_LINE.findall(netsh_output)]


NETSH_EXCLUDED_PORTS_COMMAND = ["netsh", "int", "ipv4", "show", "excludedportrange", "protocol=tcp"]
NETSH_TIMEOUT_SECONDS = 30


def read_excluded_port_ranges() -> list[PortRange] | None:
    """Return the TCP port ranges Windows excludes (Hyper-V/WSL reserve them), or None when netsh can't tell."""
    try:
        output = subprocess.run(NETSH_EXCLUDED_PORTS_COMMAND, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, errors="replace", timeout=NETSH_TIMEOUT_SECONDS, check=True).stdout
    except (OSError, subprocess.SubprocessError) as error:
        LOG.warning("Can't read the Windows excluded port ranges with netsh: %s", error)
        return None
    ranges = parse_excluded_port_ranges(output)
    LOG.debug("Windows excluded TCP port ranges: %s", ", ".join(describe_range(r) for r in ranges) or "none")
    return ranges


SERVER_PORT_VARIABLE = "SCCACHE_SERVER_PORT"
DEFAULT_SERVER_PORT = 4226  # sccache's own default.
# Tried in order when the default is excluded. Fixed, not random, so a server an earlier run started is reused.
FALLBACK_SERVER_PORTS = (4400, 7226, 9226, 14226, 24226, 34226)


@dataclass(frozen=True)
class ServerPortChoice:
    """The port the sccache server listens on and the line that says why."""

    port: int
    message: str
    level: int


def excluding_range(port: int, excluded: list[PortRange]) -> PortRange | None:
    """Return the excluded range that covers a port, or None when none does."""
    return next((port_range for port_range in excluded if port in port_range), None)


def describe_range(port_range: PortRange) -> str:
    """Return a port range as netsh lists it, e.g. 4158-4257."""
    return f"{port_range.first}-{port_range.last}"


def choose_server_port(*, configured: str | None, excluded: list[PortRange] | None) -> ServerPortChoice:
    """Choose the sccache server port: the user's SCCACHE_SERVER_PORT, else the first port Windows doesn't exclude.

    `excluded` is None when the excluded port ranges couldn't be read.
    """
    if configured:
        return user_server_port(configured, excluded or [])
    if excluded is None:
        return ServerPortChoice(DEFAULT_SERVER_PORT, f"sccache: server port {DEFAULT_SERVER_PORT} (default); "
                                "Windows port exclusions are unknown, so if the server can't start, set "
                                f"{SERVER_PORT_VARIABLE} to a port outside them", logging.WARNING)
    blocking = excluding_range(DEFAULT_SERVER_PORT, excluded)
    if blocking is None:
        return ServerPortChoice(DEFAULT_SERVER_PORT, f"sccache: server port {DEFAULT_SERVER_PORT} (default)",
                                logging.INFO)
    for port in FALLBACK_SERVER_PORTS:
        if excluding_range(port, excluded) is None:
            return ServerPortChoice(port, f"sccache: server port {port} (default {DEFAULT_SERVER_PORT} is in "
                                    f"the Windows excluded port range {describe_range(blocking)})", logging.INFO)
    return ServerPortChoice(DEFAULT_SERVER_PORT, f"sccache: server port {DEFAULT_SERVER_PORT} (default), although "
                            "Windows excludes it and every fallback port; set "
                            f"{SERVER_PORT_VARIABLE} to a port outside the excluded ranges", logging.WARNING)


def user_server_port(configured: str, excluded: list[PortRange]) -> ServerPortChoice:
    """Keep the port the user set, warning when Windows excludes it."""
    try:
        port = int(configured)
    except ValueError as error:
        raise ValueError(f"{SERVER_PORT_VARIABLE} must be a port number, not {configured!r}") from error
    blocking = excluding_range(port, excluded)
    if blocking is None:
        return ServerPortChoice(port, f"sccache: server port {port} (set by {SERVER_PORT_VARIABLE})", logging.INFO)
    return ServerPortChoice(port, f"sccache: server port {port} (set by {SERVER_PORT_VARIABLE}), although it is "
                            f"in the Windows excluded port range {describe_range(blocking)}", logging.WARNING)


def server_start_problem(exit_code: int, output: str, *, server_answers: bool, port: int) -> str | None:
    """Return why the build can't compile through sccache after `sccache --start-server`, or None when it can.

    A failed start is fine when a server answers: one was already running.
    """
    if exit_code == 0 or server_answers:
        return None
    cause_lines = [line.strip() for line in output.splitlines()
                   if line.strip() and not line.startswith("sccache: Starting the server")]
    cause = " ".join(cause_lines) or f"sccache --start-server failed with exit code {exit_code} and no output"
    return (f"The sccache server could not start on port {port}: {cause} "
            f"Build without it with --no-sccache, or set {SERVER_PORT_VARIABLE} to a free port "
            "outside the ranges `netsh int ipv4 show excludedportrange protocol=tcp` lists.")


SERVER_HOST = "127.0.0.1"
CONNECT_TIMEOUT_SECONDS = 2


def server_listens(port: int) -> bool:
    """Return whether something accepts connections on the sccache server port.

    `sccache --show-stats` can't tell: with no server it still exits 0, printing zeroed stats.
    """
    try:
        with socket.create_connection((SERVER_HOST, port), timeout=CONNECT_TIMEOUT_SECONDS):
            return True
    except OSError:
        return False


SCCACHE_TIMEOUT_SECONDS = 60


class CompilerCache:
    """The sccache server a build compiles through: started before the build, its stats read around it."""

    def __init__(self, sccache: Path, environment: dict[str, str]) -> None:
        self.sccache = sccache
        self.environment = environment
        self.before: dict | None = None

    def start(self) -> None:
        """Start the server with our cache settings and take stats; warn when a running one has other settings.

        Raises RuntimeError when no server answers, so the build stops before every compile fails.
        """
        started = time.monotonic()
        # The server outlives this run (other worktrees share it), so it must not hold our pipes open:
        # its output goes through a file instead.
        with tempfile.TemporaryFile() as output_file:
            result = subprocess.run([str(self.sccache), "--start-server"], env=self.environment,
                                    stdin=subprocess.DEVNULL, stdout=output_file, stderr=subprocess.STDOUT,
                                    timeout=SCCACHE_TIMEOUT_SECONDS, check=False)
            output_file.seek(0)
            output = output_file.read().decode("utf-8", errors="replace")
        LOG.debug("sccache --start-server exited with %s after %.1f s: %s", result.returncode,
                  time.monotonic() - started, output.strip())
        port = int(self.environment.get(SERVER_PORT_VARIABLE, DEFAULT_SERVER_PORT))
        start_problem = server_start_problem(result.returncode, output, server_answers=server_listens(port),
                                             port=port)
        if start_problem:
            raise RuntimeError(start_problem)
        self.before = self.read_stats()
        if self.before is None:
            return
        settings_problem = server_settings_problem(self.before, self.environment)
        if settings_problem:
            LOG.warning(settings_problem)

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
