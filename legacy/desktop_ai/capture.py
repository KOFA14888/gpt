"""Live screen capture at a configurable frame rate (5 FPS by default).

Can capture the whole virtual desktop or only a user-selected region.
Our own overlay windows are masked out so the assistant never sees itself.
"""

from __future__ import annotations

import io
import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Optional

from PIL import Image, ImageDraw

try:
    from PIL import ImageGrab
except Exception:  # pragma: no cover
    ImageGrab = None  # type: ignore

from PySide6.QtCore import QObject, Signal

try:
    import mss  # type: ignore
except Exception:  # pragma: no cover - mss is optional, PIL is the fallback
    mss = None  # type: ignore


@dataclass(frozen=True)
class Frame:
    jpeg: bytes
    width: int
    height: int
    captured_at: float
    fps: float
    origin_x: int = 0  # global coordinate of the captured image's top-left
    origin_y: int = 0


class ScreenCapture(QObject):
    """Capture loop running in its own daemon thread."""

    frame_ready = Signal(object)
    error = Signal(str)

    def __init__(self, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.RLock()
        self._fps = 5
        self._quality = 70
        self._max_side = 1600
        self._excludes: list[tuple[int, int, int, int]] = []
        self._region: Optional[tuple[int, int, int, int]] = None
        self._last: Optional[Frame] = None
        self._times: deque[float] = deque(maxlen=12)
        self._last_error_at = 0.0

    # ---------- configuration ----------

    def configure(self, fps: int, quality: int, max_side: int) -> None:
        with self._lock:
            self._fps = max(1, min(15, int(fps)))
            self._quality = max(35, min(95, int(quality)))
            self._max_side = max(640, min(2560, int(max_side)))

    def set_exclude_rects(self, rects: list[tuple[int, int, int, int]]) -> None:
        """Global-coordinate rectangles to paint over (our own windows)."""
        with self._lock:
            self._excludes = list(rects)

    def set_exclude_rect(self, rect: Optional[tuple[int, int, int, int]]) -> None:
        self.set_exclude_rects([rect] if rect else [])

    def set_region(self, rect: Optional[tuple[int, int, int, int]]) -> None:
        """Capture only this global rectangle; None = whole virtual desktop."""
        with self._lock:
            self._region = tuple(rect) if rect else None  # type: ignore[assignment]

    @property
    def region(self) -> Optional[tuple[int, int, int, int]]:
        with self._lock:
            return self._region

    # ---------- lifecycle ----------

    def start(self) -> None:
        with self._lock:
            if self._thread and self._thread.is_alive():
                return
            self._stop.clear()
            self._thread = threading.Thread(target=self._loop, name="screen-capture", daemon=True)
            self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        thread = self._thread
        if thread and thread.is_alive() and thread is not threading.current_thread():
            thread.join(timeout=2.0)
        with self._lock:
            self._thread = None

    @property
    def running(self) -> bool:
        return bool(self._thread and self._thread.is_alive() and not self._stop.is_set())

    def latest(self) -> Optional[Frame]:
        with self._lock:
            return self._last

    @property
    def fps(self) -> float:
        with self._lock:
            if len(self._times) < 2:
                return 0.0
            stamps = list(self._times)
            span = stamps[-1] - stamps[0]
            return (len(stamps) - 1) / span if span > 0 else 0.0

    # ---------- internals ----------

    def _loop(self) -> None:
        grabber = None
        desktop_left = desktop_top = 0
        try:
            if mss is not None:
                grabber = mss.mss()
                monitor = grabber.monitors[0]  # virtual desktop: covers all monitors
                desktop_left, desktop_top = int(monitor["left"]), int(monitor["top"])
        except Exception as exc:  # pragma: no cover - depends on the OS
            grabber = None
            self._report_error(f"mss недоступен, используется PIL: {exc}")

        next_tick = time.monotonic()
        while not self._stop.is_set():
            started = time.monotonic()
            try:
                frame = self._grab(grabber, desktop_left, desktop_top)
                if frame is not None:
                    with self._lock:
                        self._last = frame
                        self._times.append(started)
                    self.frame_ready.emit(frame)
            except Exception as exc:
                self._report_error(f"Не удалось захватить экран: {exc}")

            with self._lock:
                interval = 1.0 / self._fps
            next_tick += interval
            delay = next_tick - time.monotonic()
            if delay < 0:  # do not spin after a long stall
                next_tick = time.monotonic() + interval
                delay = interval
            if self._stop.wait(delay):
                break
        if grabber is not None:
            try:
                grabber.close()
            except Exception:
                pass

    def _report_error(self, message: str) -> None:
        now = time.monotonic()
        with self._lock:
            if now - self._last_error_at < 10:
                return
            self._last_error_at = now
        self.error.emit(message)

    def _grab(self, grabber, desktop_left: int, desktop_top: int) -> Optional[Frame]:
        with self._lock:
            quality = self._quality
            max_side = self._max_side
            excludes = list(self._excludes)
            region = self._region

        origin_x = origin_y = 0
        if region:
            x, y, w, h = region
            origin_x, origin_y = x, y
            if grabber is not None:
                shot = grabber.grab({"left": x, "top": y, "width": w, "height": h})
                image = Image.frombytes("RGB", (w, h), shot.bgra, "raw", "BGRX")
            else:  # pragma: no cover - platform fallback
                if ImageGrab is None:
                    return None
                image = ImageGrab.grab(
                    bbox=(x - desktop_left, y - desktop_top, x - desktop_left + w, y - desktop_top + h),
                    all_screens=True,
                ).convert("RGB")
        else:
            origin_x, origin_y = desktop_left, desktop_top
            if grabber is not None:
                monitor = grabber.monitors[0]
                shot = grabber.grab(monitor)
                image = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
            else:  # pragma: no cover - platform fallback
                if ImageGrab is None:
                    return None
                image = ImageGrab.grab(all_screens=True).convert("RGB")

        draw = ImageDraw.Draw(image)
        for ex, ey, ew, eh in excludes:
            # Clip the overlay rectangle to the captured area.
            box = (
                ex - origin_x,
                ey - origin_y,
                ex + ew - origin_x,
                ey + eh - origin_y,
            )
            box = (
                max(0, box[0]),
                max(0, box[1]),
                min(image.width, box[2]),
                min(image.height, box[3]),
            )
            if box[2] > box[0] and box[3] > box[1]:
                draw.rectangle(box, fill=(24, 26, 36))

        image.thumbnail((max_side, max_side), Image.Resampling.LANCZOS)
        buffer = io.BytesIO()
        image.save(buffer, format="JPEG", quality=quality, optimize=False)
        return Frame(
            jpeg=buffer.getvalue(),
            width=image.width,
            height=image.height,
            captured_at=time.time(),
            fps=self.fps,
            origin_x=origin_x,
            origin_y=origin_y,
        )
