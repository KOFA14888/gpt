"""Crash/heartbeat logging. Everything goes to %APPDATA%/screenmate_ai/crash.log."""

from __future__ import annotations

import faulthandler
from datetime import datetime

from .config import CONFIG_FILE

LOG_FILE = CONFIG_FILE.parent / "crash.log"
_fault_file = None


def log_event(message: str) -> None:
    try:
        with LOG_FILE.open("a", encoding="utf-8") as fh:
            fh.write(f"{datetime.now().isoformat(timespec='seconds')}  {message}\n")
    except OSError:
        pass


def setup_diagnostics() -> None:
    """Capture even hard crashes (access violations) in a log file."""
    global _fault_file
    try:
        _fault_file = LOG_FILE.open("a", encoding="utf-8", buffering=1)
        faulthandler.enable(file=_fault_file)
        log_event("--- session start ---")
    except Exception:
        pass
