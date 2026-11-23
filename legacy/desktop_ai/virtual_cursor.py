"""A visible, separate cursor that the AI animates and clicks with."""

from __future__ import annotations

from typing import Callable, Optional

from PySide6.QtCore import QEasingCurve, QPoint, QPropertyAnimation, QTimer, Qt
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QWidget

from .actions import cursor_pos


class VirtualCursor(QWidget):
    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setWindowFlags(
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool | Qt.WindowTransparentForInput
        )
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setAttribute(Qt.WA_TransparentForMouseEvents)
        self.resize(32, 32)
        self._animation: Optional[QPropertyAnimation] = None
        self._callback: Optional[Callable[[], None]] = None
        self._flash = 0
        self.hide()

    # ---------- geometry ----------

    def center_pos(self) -> QPoint:
        return self.pos() + QPoint(self.width() // 2, self.height() // 2)

    def animate_to(self, x: int, y: int, callback: Optional[Callable[[], None]] = None) -> None:
        target = QPoint(int(x) - self.width() // 2, int(y) - self.height() // 2)
        self._callback = callback
        if self._animation is not None:
            self._animation.stop()

        if not self.isVisible():
            # Start from the user's real cursor so the first move is smooth too.
            start_x, start_y = cursor_pos()
            self.move(start_x - self.width() // 2, start_y - self.height() // 2)
            self.show()

        animation = QPropertyAnimation(self, b"pos")
        animation.setDuration(420)
        animation.setEasingCurve(QEasingCurve.InOutQuad)
        animation.setEndValue(target)
        animation.finished.connect(self._on_finished)
        self._animation = animation
        animation.start()

    def _on_finished(self) -> None:
        callback, self._callback = self._callback, None
        if callback:
            callback()

    def flash(self) -> None:
        self._flash = 7
        self.update()
        QTimer.singleShot(170, self._clear_flash)

    def _clear_flash(self) -> None:
        self._flash = 0
        self.update()

    # ---------- painting ----------

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt naming
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        if self._flash:
            painter.setPen(QPen(QColor(140, 120, 255, 170), 2))
            painter.setBrush(Qt.NoBrush)
            center = QPoint(self.width() // 2, self.height() // 2)
            radius = 8 + self._flash
            painter.drawEllipse(center, radius, radius)

        # Classic Windows-style arrow pointer: white fill, black outline.
        path = QPainterPath()
        path.moveTo(3, 2)
        path.lineTo(3, 24)
        path.lineTo(8.5, 18.5)
        path.lineTo(12, 27)
        path.lineTo(15.5, 25.5)
        path.lineTo(12, 17.5)
        path.lineTo(20, 17.5)
        path.closeSubpath()

        # Soft drop shadow so the arrow stays visible on any background.
        shadow = QPainterPath(path)
        shadow.translate(1.2, 1.2)
        painter.setPen(Qt.NoPen)
        painter.setBrush(QColor(0, 0, 0, 120))
        painter.drawPath(shadow)

        painter.setPen(QPen(QColor("#000000"), 1.4, Qt.SolidLine, Qt.RoundJoin))
        painter.setBrush(QColor("#ffffff"))
        painter.drawPath(path)
        painter.end()
