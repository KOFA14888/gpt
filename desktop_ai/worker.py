"""Threaded LLM request worker.

Uses a plain Python thread and a queue instead of QThread/QObject lifetimes:
PySide6 can crash silently when a QObject living in a finished thread is
deleted from the GUI thread. GUI code drains ``events`` with a QTimer.
"""

from __future__ import annotations

import queue
import threading
from typing import Optional

from .providers import CancelledError, ask_stream


class LLMWorker:
    def __init__(
        self,
        config,
        question: str,
        frame_jpeg: Optional[bytes],
        history: list[dict[str, str]],
    ) -> None:
        self.config = config
        self.question = question
        self.frame_jpeg = frame_jpeg
        self.history = history
        self.events: queue.Queue[tuple[str, str]] = queue.Queue()
        self._cancel = threading.Event()
        self.thread = threading.Thread(target=self.run, name="llm-request", daemon=True)

    def start(self) -> None:
        self.thread.start()

    def cancel(self) -> None:
        self._cancel.set()

    def _post(self, kind: str, payload: str = "") -> None:
        self.events.put((kind, payload))

    def run(self) -> None:
        try:
            answer = ask_stream(
                config=self.config,
                question=self.question,
                frame_jpeg=self.frame_jpeg,
                history=self.history,
                on_chunk=lambda text: self._post("chunk", text),
                cancelled=self._cancel.is_set,
            )
        except CancelledError:
            self._post("cancelled")
            return
        except Exception as exc:  # noqa: BLE001 - show any API failure in chat
            self._post("failed", str(exc))
            return
        self._post("done", answer)
