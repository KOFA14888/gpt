"""Application entry point."""

from __future__ import annotations

import sys
import traceback

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from .capture import ScreenCapture
from .config import Config
from .diag import log_event, setup_diagnostics
from .ui import STYLE_SHEET, ChatOverlay


def _excepthook(exc_type, exc, tb) -> None:
    log_event("PYTHON ERROR:\n" + "".join(traceback.format_exception(exc_type, exc, tb)))
    sys.__excepthook__(exc_type, exc, tb)


def main() -> int:
    setup_diagnostics()
    sys.excepthook = _excepthook
    if hasattr(QApplication, "setHighDpiScaleFactorRoundingPolicy"):
        QApplication.setHighDpiScaleFactorRoundingPolicy(
            Qt.HighDpiScaleFactorRoundingPolicy.PassThrough
        )
    app = QApplication(sys.argv)
    app.setApplicationName("ScreenMate AI")
    app.setQuitOnLastWindowClosed(False)
    app.setStyleSheet(STYLE_SHEET)

    config = Config.load()
    capture = ScreenCapture()
    overlay = ChatOverlay(config, capture)

    capture.start()
    overlay.show()
    overlay.raise_()
    log_event(f"started, provider={config.provider}, model={config.model}, fps={config.fps}")

    try:
        return app.exec()
    except BaseException:
        log_event("FATAL:\n" + traceback.format_exc())
        raise
    finally:
        capture.stop()
        log_event("stopped")


if __name__ == "__main__":
    raise SystemExit(main())
