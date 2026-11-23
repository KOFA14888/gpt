"""Interactive selection of the screen area the AI is allowed to see."""

from __future__ import annotations

from PySide6.QtCore import QPoint, QRect, Qt, Signal
from PySide6.QtGui import QColor, QFont, QMouseEvent, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QApplication, QWidget

HANDLES = ("nw", "n", "ne", "e", "se", "s", "sw", "w")
MIN_W, MIN_H = 120, 80


class RegionSelector(QWidget):
    """Full-screen transparent layer with a draggable/resizable rectangle."""

    accepted = Signal(object)  # global QRect
    rejected = Signal()

    def __init__(self, initial: QRect | None = None, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        screen = QApplication.primaryScreen()
        desktop = screen.virtualGeometry() if screen else QRect(0, 0, 1920, 1080)
        self.setGeometry(desktop)
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool)
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setCursor(Qt.CrossCursor)
        self.setMouseTracking(True)

        if initial and initial.isValid():
            self._rect = QRect(
                initial.left() - desktop.left(),
                initial.top() - desktop.top(),
                initial.width(),
                initial.height(),
            )
        else:
            w = max(MIN_W * 3, min(desktop.width() // 2, 900))
            h = max(MIN_H * 3, min(desktop.height() // 2, 560))
            self._rect = QRect((desktop.width() - w) // 2, (desktop.height() - h) // 2, w, h)

        self._mode: str | None = None
        self._anchor = QPoint()
        self._start = QRect(self._rect)

    # ---------- helpers ----------

    def _handle_points(self) -> dict[str, QPoint]:
        r = self._rect
        return {
            "nw": QPoint(r.left(), r.top()),
            "n": QPoint(r.center().x(), r.top()),
            "ne": QPoint(r.right(), r.top()),
            "e": QPoint(r.right(), r.center().y()),
            "se": QPoint(r.right(), r.bottom()),
            "s": QPoint(r.center().x(), r.bottom()),
            "sw": QPoint(r.left(), r.bottom()),
            "w": QPoint(r.left(), r.center().y()),
        }

    def _hit(self, pos: QPoint) -> str | None:
        for name, point in self._handle_points().items():
            if abs(pos.x() - point.x()) <= 10 and abs(pos.y() - point.y()) <= 10:
                return name
        if self._rect.contains(pos):
            return "move"
        return "new"

    def _cursor_for(self, mode: str | None) -> Qt.CursorShape:
        return {
            "nw": Qt.SizeFDiagCursor,
            "se": Qt.SizeFDiagCursor,
            "ne": Qt.SizeBDiagCursor,
            "sw": Qt.SizeBDiagCursor,
            "n": Qt.SizeVerCursor,
            "s": Qt.SizeVerCursor,
            "e": Qt.SizeHorCursor,
            "w": Qt.SizeHorCursor,
            "move": Qt.OpenHandCursor,
            "new": Qt.CrossCursor,
        }.get(mode or "", Qt.CrossCursor)

    def _clamp(self) -> None:
        self._rect = self._rect.intersected(self.rect())
        if self._rect.width() < MIN_W:
            self._rect.setWidth(min(MIN_W, self.rect().width()))
        if self._rect.height() < MIN_H:
            self._rect.setHeight(min(MIN_H, self.rect().height()))

    # ---------- events ----------

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt naming
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        outside = QPainterPath()
        outside.addRect(self.rect())
        inside = QPainterPath()
        inside.addRect(self._rect)
        painter.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 120))

        pen = QPen(QColor("#8b7bff"), 2)
        pen.setDashPattern([6, 4])
        painter.setPen(pen)
        painter.setBrush(Qt.NoBrush)
        painter.drawRect(self._rect)

        painter.setPen(Qt.NoPen)
        painter.setBrush(QColor("#ffffff"))
        for point in self._handle_points().values():
            painter.drawRect(point.x() - 4, point.y() - 4, 8, 8)

        text = (
            f"  ОБЛАСТЬ ДЛЯ ИИ   {self._rect.width()}×{self._rect.height()}   "
            "оно — Enter   отмена — Esc  "
        )
        font = QFont("Segoe UI", 10, QFont.Bold)
        painter.setFont(font)
        box = painter.fontMetrics().boundingRect(text).adjusted(-4, -3, 4, 3)
        box.moveTopLeft(QPoint(self._rect.left(), max(0, self._rect.top() - 32)))
        painter.setBrush(QColor(20, 22, 34, 235))
        painter.drawRoundedRect(box, 6, 6)
        painter.setPen(QColor("#e9ecf5"))
        painter.drawText(box, Qt.AlignVCenter, text)
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() != Qt.LeftButton:
            return
        self._mode = self._hit(event.position().toPoint())
        self._anchor = event.position().toPoint()
        self._start = QRect(self._rect)
        if self._mode == "new":
            self._rect = QRect(self._anchor, self._anchor)
        self.setCursor(self._cursor_for(self._mode))
        event.accept()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        pos = event.position().toPoint()
        if not self._mode or not (event.buttons() & Qt.LeftButton):
            self.setCursor(self._cursor_for(self._hit(pos)))
            return
        dx = pos.x() - self._anchor.x()
        dy = pos.y() - self._anchor.y()
        r = QRect(self._start)

        if self._mode == "move":
            r.translate(dx, dy)
        elif self._mode == "new":
            r = QRect(self._anchor, pos).normalized()
        else:
            if "n" in self._mode:
                r.setTop(r.top() + dy)
            if "s" in self._mode:
                r.setBottom(r.bottom() + dy)
            if "w" in self._mode:
                r.setLeft(r.left() + dx)
            if "e" in self._mode:
                r.setRight(r.right() + dx)
            if r.width() < MIN_W:
                if "w" in self._mode:
                    r.setLeft(r.right() - MIN_W)
                else:
                    r.setWidth(MIN_W)
            if r.height() < MIN_H:
                if "n" in self._mode:
                    r.setTop(r.bottom() - MIN_H)
                else:
                    r.setHeight(MIN_H)
        self._rect = r
        self._clamp()
        self.update()
        event.accept()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        self._mode = None
        self.setCursor(Qt.CrossCursor)
        event.accept()

    def keyPressEvent(self, event) -> None:  # noqa: N802
        key = event.key()
        if key == Qt.Key_Escape:
            self.rejected.emit()
            self.close()
            return
        if key in (Qt.Key_Return, Qt.Key_Enter):
            self.accept_selection()
            return

        step = 1 if event.modifiers() & Qt.ShiftModifier else 10
        dx = dy = 0
        if key == Qt.Key_Left:
            dx = -step
        elif key == Qt.Key_Right:
            dx = step
        elif key == Qt.Key_Up:
            dy = -step
        elif key == Qt.Key_Down:
            dy = step
        else:
            super().keyPressEvent(event)
            return

        if event.modifiers() & Qt.ControlModifier:
            self._rect.adjust(0, 0, dx, dy)
        else:
            self._rect.translate(dx, dy)
        self._clamp()
        self.update()
        event.accept()

    def accept_selection(self) -> None:
        geo = self.geometry()
        global_rect = QRect(
            self._rect.left() + geo.left(),
            self._rect.top() + geo.top(),
            self._rect.width(),
            self._rect.height(),
        )
        self.accepted.emit(global_rect)
        self.close()
