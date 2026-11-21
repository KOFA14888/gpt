"""Application entry point."""

from __future__ import annotations

import sys

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from .capture import ScreenCapture
from .config import Config
from .ui import STYLE_SHEET, ChatOverlay


def main() -> int:
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

    try:
        return app.exec()
    finally:
        capture.stop()


if __name__ == "__main__":
    raise SystemExit(main())
