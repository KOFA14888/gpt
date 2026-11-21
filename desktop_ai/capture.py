"""Live screen capture at a configurable frame rate (5 FPS by default).

The overlay window itself is masked out of every frame, so the assistant
never sees a feedback loop of its own chat.
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
        self._exclude: Optional[tuple[int, int, int, int]] = None
        self._last: Optional[Frame] = None
        self._times: deque[float] = deque(maxlen=12)
        self._last_error_at = 0.0

    # ---------- configuration ----------

    def configure(self, fps: int, quality: int, max_side: int) -> None:
        with self._lock:
            self._fps = max(1, min(15, int(fps)))
            self._quality = max(35, min(95, int(quality)))
            self._max_side = max(640, min(2560, int(max_side)))

    def set_exclude_rect(self, rect: Optional[tuple[int, int, int, int]]) -> None:
        """Global screen coordinates of the region to paint over (our own window)."""
        with self._lock:
            self._exclude = rect

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
        left = top = 0
        try:
            if mss is not None:
                grabber = mss.mss()
                monitor = grabber.monitors[0]  # virtual desktop: covers all monitors
                left, top = int(monitor["left"]), int(monitor["top"])
        except Exception as exc:  # pragma: no cover - depends on the OS
            grabber = None
            self._report_error(f"mss недоступен, используется PIL: {exc}")

        next_tick = time.monotonic()
        while not self._stop.is_set():
            started = time.monotonic()
            try:
                frame = self._grab(grabber, left, top)
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

    def _grab(self, grabber, left: int, top: int) -> Optional[Frame]:
        with self._lock:
            quality = self._quality
            max_side = self._max_side
            exclude = self._exclude

        if grabber is not None:
            monitor = grabber.monitors[0]
            shot = grabber.grab(monitor)
            image = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
        else:  # pragma: no cover - platform fallback
            image = ImageGrab.grab(all_screens=True).convert("RGB")
            left = top = 0

        if exclude:
            x, y, w, h = exclude
            box = (x - left, y - top, x - left + w, y - top + h)
            draw = ImageDraw.Draw(image)
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
        )
