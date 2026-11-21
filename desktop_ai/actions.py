"""Parse cursor commands from model output and execute them on Windows."""

from __future__ import annotations

import os
import re
import time
from dataclasses import dataclass
from typing import Optional

COMMAND_RE = re.compile(
    r"\[\s*(MOVE|CLICK|RIGHT_CLICK|DOUBLE_CLICK)\s*"
    r"(?:(-?\d+)\s*[, ]\s*(-?\d+))?\s*\]",
    re.IGNORECASE,
)
# A command that has not been streamed in full yet — hide it while typing.
PARTIAL_RE = re.compile(r"\[[A-Z_]*,?\s*[-\d,\s]*$")


@dataclass(frozen=True)
class Action:
    kind: str
    x: Optional[int] = None
    y: Optional[int] = None


def parse_commands(text: str) -> tuple[str, list[Action]]:
    actions: list[Action] = []
    matches = []

    for match in COMMAND_RE.finditer(text):
        kind = match.group(1).upper()
        x = int(match.group(2)) if match.group(2) is not None else None
        y = int(match.group(3)) if match.group(3) is not None else None
        if kind in {"MOVE", "CLICK", "RIGHT_CLICK", "DOUBLE_CLICK"}:
            if kind == "MOVE" and (x is None or y is None):
                continue
            actions.append(Action(kind, x, y))
            matches.append(match)

    clean = text
    for match in reversed(matches):
        clean = clean[: match.start()] + clean[match.end() :]
    clean = re.sub(r"[ \t]+\n", "\n", clean)
    clean = re.sub(r" {2,}", " ", clean)
    clean = re.sub(r"\n{3,}", "\n\n", clean).strip()
    return clean, actions


def clean_stream_preview(text: str) -> str:
    """Hide fully formed and half-typed commands from the chat bubble."""
    clean, _ = parse_commands(text)
    return PARTIAL_RE.sub("", clean).rstrip()


# ---------- Windows mouse control ----------

if os.name == "nt":  # pragma: no cover - Windows only
    import ctypes

    _user32 = ctypes.windll.user32

    class POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

    LEFT_DOWN, LEFT_UP = 0x0002, 0x0004
    RIGHT_DOWN, RIGHT_UP = 0x0008, 0x0010
else:  # pragma: no cover - headless tests run on Linux
    _user32 = None
    POINT = None  # type: ignore[assignment]
    LEFT_DOWN = LEFT_UP = RIGHT_DOWN = RIGHT_UP = 0


def mouse_available() -> bool:
    return _user32 is not None


def cursor_pos() -> tuple[int, int]:
    if not mouse_available():
        return 0, 0
    point = POINT()
    _user32.GetCursorPos(ctypes.byref(point))
    return point.x, point.y


def set_cursor_pos(x: int, y: int) -> None:
    if mouse_available():
        _user32.SetCursorPos(int(x), int(y))


def click_at(x: int, y: int, kind: str = "CLICK") -> None:
    """Click at (x, y), then put the user's real cursor back where it was."""
    if not mouse_available():
        return
    saved_x, saved_y = cursor_pos()
    set_cursor_pos(x, y)
    time.sleep(0.04)

    down, up = (RIGHT_DOWN, RIGHT_UP) if kind == "RIGHT_CLICK" else (LEFT_DOWN, LEFT_UP)
    count = 2 if kind == "DOUBLE_CLICK" else 1
    for _ in range(count):
        _user32.mouse_event(down, 0, 0, 0, 0)
        time.sleep(0.05)
        _user32.mouse_event(up, 0, 0, 0, 0)
        time.sleep(0.05)

    set_cursor_pos(saved_x, saved_y)
