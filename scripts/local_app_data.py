"""Locate %LOCALAPPDATA% and the JFG folder under it, which all local build output uses.

Fork-only. Shared by dev_build.py, dev_bundle.py and the generated-code store,
so each reports a missing LOCALAPPDATA the same way.
"""
from __future__ import annotations

import os
from pathlib import Path

JFG_FOLDER_NAME = "JFG"


def local_app_data() -> Path:
    r"""Return %LOCALAPPDATA%; fail with a readable error when it is not set."""
    local = os.environ.get("LOCALAPPDATA")
    if not local:
        raise ValueError(r"LOCALAPPDATA is not set, so %LOCALAPPDATA%\JFG (local build output) and the "
                         "game's data folder can't be found.")
    return Path(local)


def jfg_root() -> Path:
    r"""Return %LOCALAPPDATA%\JFG, the root all local build output lives under."""
    return local_app_data() / JFG_FOLDER_NAME
