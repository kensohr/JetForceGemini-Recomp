"""MSVC toolchain discovery for fork development builds (scripts/dev_build.py).

Fork-only. Finds the Visual Studio 2022 install with C++ tools through vswhere,
its vcvars64.bat and bundled ninja.exe, and captures the x64 MSVC environment
vcvars64.bat leaves.
"""
from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import subprocess


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
