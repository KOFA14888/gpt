"""Always-on-top chat overlay and settings dialog (PySide6)."""

from __future__ import annotations

import copy
import html
import queue
import threading
import traceback
from typing import Optional

from PySide6.QtCore import QPoint, QRect, QTimer, Qt
from PySide6.QtGui import (
    QAction,
    QColor,
    QCursor,
    QIcon,
    QImage,
    QPainter,
    QPen,
    QPixmap,
)
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFrame,
    QGraphicsDropShadowEffect,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMenu,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSpinBox,
    QSystemTrayIcon,
    QVBoxLayout,
    QWidget,
)

from .capture import Frame, ScreenCapture
from .config import Config
from .diag import log_event
from .worker import LLMWorker
from .providers import CancelledError, ModelInfo, ask_stream, list_models

STYLE_SHEET = """
QWidget#root { background: transparent; }
QFrame#card {
    background: rgba(17, 19, 28, 248);
    border: 1px solid rgba(255, 255, 255, 32);
    border-radius: 16px;
}
QLabel { color: #e9ecf5; font-family: 'Segoe UI', 'Inter', sans-serif; font-size: 12px; }
QLabel#title { font-size: 13px; font-weight: 700; letter-spacing: 1px; }
QLabel#meta { color: #8e96ae; font-size: 11px; }
QLabel#preview {
    background: #10131e; border-radius: 10px; color: #68718b;
    qproperty-alignment: AlignCenter;
}
QLabel#live {
    background: rgba(10, 12, 18, 190); color: #6ee7a8; border-radius: 6px;
    padding: 2px 6px; font-size: 10px; font-weight: 700;
}
QPushButton {
    background: transparent; color: #aab2c8; border: none; border-radius: 7px;
    min-width: 26px; min-height: 24px; font-size: 14px;
}
QPushButton:hover { background: rgba(255, 255, 255, 20); color: #ffffff; }
QPushButton#send {
    background: #6c5ce7; color: white; font-size: 15px; min-width: 34px;
    min-height: 32px; border-radius: 10px; font-weight: 700;
}
QPushButton#send:hover { background: #7d6ff2; }
QPushButton#send:disabled { background: #3b3c4d; color: #71758c; }
QLineEdit#input {
    background: rgba(255, 255, 255, 10); border: 1px solid rgba(255, 255, 255, 22);
    border-radius: 10px; padding: 7px 10px; color: #f2f4fa; font-size: 12px;
    selection-background-color: #6c5ce7;
}
QLineEdit#input:focus { border: 1px solid #6c5ce7; }
QScrollArea#chat { background: transparent; border: none; }
QWidget#chat_contents { background: transparent; }
QFrame#bubble_ai, QFrame#bubble_mine { border-radius: 11px; }
QFrame#bubble_ai { background: rgba(255, 255, 255, 12); }
QFrame#bubble_mine { background: #574bd0; }
QFrame#bubble_error { background: rgba(190, 70, 80, 45); border-radius: 11px; }
QLabel#message { font-size: 12px; line-height: 145%; }
QLabel#system { color: #a9b0c4; font-size: 11px; }
QLabel#grip { color: #4a5063; qproperty-alignment: AlignRight; }
QDialog { background: #141722; color: #e9ecf5; font-family: 'Segoe UI', sans-serif; }
QComboBox, QLineEdit, QSpinBox, QPlainTextEdit {
    background: #202433; border: 1px solid #343a50; border-radius: 7px;
    padding: 5px 8px; color: #eef0f8; selection-background-color: #6c5ce7;
}
QPlainTextEdit { min-height: 90px; }
QLabel#hint, QLabel#status { color: #8e96ae; font-size: 11px; }
QPushButton#small { min-width: 66px; min-height: 26px; background: #2b3045; }
QCheckBox { color: #d8dcea; }
"""


class MessageBubble(QFrame):
    def __init__(self, text: str, mine: bool = False, error: bool = False) -> None:
        super().__init__()
        self.setObjectName("bubble_error" if error else ("bubble_mine" if mine else "bubble_ai"))
        self._text = ""
        self.label = QLabel()
        self.label.setObjectName("message")
        self.label.setWordWrap(True)
        self.label.setTextInteractionFlags(Qt.TextSelectableByMouse | Qt.LinksAccessibleByMouse)
        self.label.setOpenExternalLinks(True)
        self.label.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(9, 7, 9, 7)
        layout.addWidget(self.label)
        self.set_text(text)

    def set_text(self, text: str) -> None:
        self._text = text
        safe = html.escape(text).replace("\n", "<br>")
        self.label.setText(safe or "&nbsp;")

    def text(self) -> str:
        return self._text


class DragBar(QFrame):
    """Header that doubles as the window drag handle."""

    def __init__(self, window: QWidget) -> None:
        super().__init__(window)
        self.window_ref = window
        self._drag_offset: Optional[QPoint] = None
        self.setObjectName("header")
        self.setCursor(QCursor(Qt.OpenHandCursor))

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.LeftButton:
            self._drag_offset = event.globalPosition().toPoint() - self.window_ref.frameGeometry().topLeft()
            self.setCursor(QCursor(Qt.ClosedHandCursor))
            event.accept()

    def mouseMoveEvent(self, event) -> None:
        if self._drag_offset is not None and event.buttons() & Qt.LeftButton:
            self.window_ref.move(event.globalPosition().toPoint() - self._drag_offset)
            event.accept()

    def mouseReleaseEvent(self, event) -> None:
        self._drag_offset = None
        self.setCursor(QCursor(Qt.OpenHandCursor))


class ResizeGrip(QLabel):
    def __init__(self, window: QWidget) -> None:
        super().__init__("◢", window)
        self.window_ref = window
        self.setObjectName("grip")
        self.setFixedHeight(15)
        self.setCursor(QCursor(Qt.SizeFDiagCursor))
        self._origin: Optional[tuple[QPoint, int, int]] = None

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.LeftButton:
            size = self.window_ref.size()
            self._origin = (event.globalPosition().toPoint(), size.width(), size.height())
            event.accept()

    def mouseMoveEvent(self, event) -> None:
        if not self._origin or not (event.buttons() & Qt.LeftButton):
            return
        start, width, height = self._origin
        delta = event.globalPosition().toPoint() - start
        screen = QApplication.primaryScreen().availableGeometry()
        max_w = screen.right() - self.window_ref.x()
        max_h = screen.bottom() - self.window_ref.y()
        self.window_ref.resize(
            max(320, min(max_w, width + delta.x())),
            max(360, min(max_h, height + delta.y())),
        )
        event.accept()

    def mouseReleaseEvent(self, event) -> None:
        self._origin = None


class ChatOverlay(QWidget):
    """Small frameless chat window pinned above all other windows."""

    def __init__(self, config: Config, capture: ScreenCapture) -> None:
        super().__init__()
        self.cfg = config
        self.capture = capture
        self.history: list[dict[str, str]] = []
        self._bubbles: list[MessageBubble] = []
        self._worker: Optional[LLMWorker] = None
        self._active_bubble: Optional[MessageBubble] = None
        self._quitting = False
        self._capture_warning_shown = False

        self.setObjectName("root")
        self.setWindowTitle("ScreenMate AI")
        self.setMinimumSize(320, 360)
        self._build_ui()
        self._place_bottom_right()
        self._apply_flags()
        self._setup_tray()

        self.capture.frame_ready.connect(self.on_frame)
        self.capture.error.connect(self.on_capture_error)
        self.status_timer = QTimer(self)
        self.status_timer.timeout.connect(self.update_status)
        self.status_timer.start(400)
        self.event_timer = QTimer(self)
        self.event_timer.timeout.connect(self._drain_events)
        self.event_timer.start(40)
        self.update_status()

    # ---------- construction ----------

    def _build_ui(self) -> None:
        shadow_layout = QVBoxLayout(self)
        shadow_layout.setContentsMargins(8, 8, 8, 8)
        card = QFrame()
        card.setObjectName("card")
        shadow = QGraphicsDropShadowEffect(card)
        shadow.setBlurRadius(24)
        shadow.setOffset(0, 4)
        shadow.setColor(QColor(0, 0, 0, 170))
        card.setGraphicsEffect(shadow)
        shadow_layout.addWidget(card)

        root = QVBoxLayout(card)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        self.header = DragBar(self)
        head_row = QHBoxLayout()
        head_row.setContentsMargins(12, 9, 12, 2)
        self.dot = QLabel("●")
        self.dot.setStyleSheet("color:#6ee7a8; font-size:11px;")
        title = QLabel("SCREENMATE  AI")
        title.setObjectName("title")
        head_row.addWidget(self.dot)
        head_row.addSpacing(6)
        head_row.addWidget(title)
        head_row.addStretch(1)

        self.preview_btn = QPushButton("◉")
        self.preview_btn.setToolTip("Показать / скрыть превью экрана")
        self.preview_btn.clicked.connect(self.toggle_preview)
        self.settings_btn = QPushButton("⚙")
        self.settings_btn.setToolTip("Настройки и API-ключи")
        self.settings_btn.clicked.connect(self.open_settings)
        self.pause_btn = QPushButton("❚❚")
        self.pause_btn.setToolTip("Пауза / продолжить захват")
        self.pause_btn.clicked.connect(self.toggle_pause)
        self.close_btn = QPushButton("✕")
        self.close_btn.setToolTip("Скрыть в трей")
        self.close_btn.clicked.connect(self.hide)
        for btn in (self.preview_btn, self.settings_btn, self.pause_btn, self.close_btn):
            head_row.addWidget(btn)
        head_row.setSpacing(2)

        meta_row = QHBoxLayout()
        meta_row.setContentsMargins(15, 0, 12, 6)
        self.live_label = QLabel("● LIVE")
        self.live_label.setObjectName("live")
        self.fps_label = QLabel("— FPS")
        self.fps_label.setObjectName("meta")
        self.provider_label = QLabel("")
        self.provider_label.setObjectName("meta")
        self.provider_label.setAlignment(Qt.AlignRight)
        meta_row.addWidget(self.live_label)
        meta_row.addSpacing(7)
        meta_row.addWidget(self.fps_label)
        meta_row.addStretch(1)
        meta_row.addWidget(self.provider_label)

        bars = QVBoxLayout(self.header)
        bars.setContentsMargins(0, 0, 0, 0)
        bars.addLayout(head_row)
        bars.addLayout(meta_row)
        root.addWidget(self.header)

        self.preview = QLabel("кадр экрана появится здесь")
        self.preview.setObjectName("preview")
        self.preview.setFixedHeight(112)
        self.preview.setScaledContents(False)
        self.preview.setAlignment(Qt.AlignCenter)
        preview_wrap = QVBoxLayout()
        preview_wrap.setContentsMargins(10, 0, 10, 8)
        preview_wrap.addWidget(self.preview)
        root.addLayout(preview_wrap)

        self.chat = QScrollArea()
        self.chat.setObjectName("chat")
        self.chat.setWidgetResizable(True)
        self.chat.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.chat.verticalScrollBar().setStyleSheet(
            "QScrollBar:vertical { width: 5px; background: transparent; }"
            "QScrollBar::handle:vertical { background: rgba(255,255,255,35); border-radius: 3px; }"
        )
        self.chat_contents = QWidget()
        self.chat_contents.setObjectName("chat_contents")
        self.chat_layout = QVBoxLayout(self.chat_contents)
        self.chat_layout.setContentsMargins(10, 4, 10, 4)
        self.chat_layout.setSpacing(7)
        self.chat_layout.addStretch(1)
        self.chat.setWidget(self.chat_contents)
        root.addWidget(self.chat, 1)

        composer = QFrame()
        composer.setContentsMargins(0, 0, 0, 0)
        comp_layout = QHBoxLayout(composer)
        comp_layout.setContentsMargins(10, 6, 10, 2)
        self.input = QLineEdit()
        self.input.setObjectName("input")
        self.input.setPlaceholderText("Спроси о том, что на экране…")
        self.input.returnPressed.connect(self.send_message)
        self.send_btn = QPushButton("➤")
        self.send_btn.setObjectName("send")
        self.send_btn.setToolTip("Отправить (Enter)")
        self.send_btn.clicked.connect(self.on_send_clicked)
        comp_layout.addWidget(self.input)
        comp_layout.addWidget(self.send_btn)
        root.addWidget(composer)

        root.addWidget(ResizeGrip(self))

    def _place_bottom_right(self) -> None:
        self.resize(380, 540)
        screen = QApplication.primaryScreen().availableGeometry()
        self.move(screen.right() - self.width() - 14, screen.bottom() - self.height() - 14)

    def _setup_tray(self) -> None:
        self.tray: Optional[QSystemTrayIcon] = None
        if not QSystemTrayIcon.isSystemTrayAvailable():
            return
        pixmap = QPixmap(64, 64)
        pixmap.fill(Qt.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.Antialiasing)
        painter.setBrush(QColor("#6c5ce7"))
        painter.setPen(Qt.NoPen)
        painter.drawRoundedRect(2, 2, 60, 60, 16, 16)
        painter.setPen(QPen(QColor("#ffffff"), 4))
        painter.setBrush(Qt.NoBrush)
        painter.drawEllipse(QRect(12, 20, 40, 24))
        painter.setBrush(QColor("#ffffff"))
        painter.drawEllipse(QRect(27, 26, 10, 12))
        painter.end()

        self.tray = QSystemTrayIcon(QIcon(pixmap), self)
        self.tray.setToolTip("ScreenMate AI — ассистент с зрением")
        menu = QMenu()
        toggle = QAction("Показать / скрыть", menu)
        toggle.triggered.connect(self.toggle_visible)
        pause = QAction("Пауза / продолжить", menu)
        pause.triggered.connect(self.toggle_pause)
        settings = QAction("Настройки", menu)
        settings.triggered.connect(self.open_settings)
        quit_action = QAction("Выход", menu)
        quit_action.triggered.connect(self.quit_app)
        menu.addAction(toggle)
        menu.addAction(pause)
        menu.addAction(settings)
        menu.addSeparator()
        menu.addAction(quit_action)
        self.tray.setContextMenu(menu)
        self.tray.activated.connect(lambda reason: self.toggle_visible() if reason == QSystemTrayIcon.Trigger else None)
        self.tray.show()

    # ---------- config / window state ----------

    def _apply_flags(self) -> None:
        flags = Qt.FramelessWindowHint | Qt.Tool
        if self.cfg.always_on_top:
            flags |= Qt.WindowStaysOnTopHint
        was_visible = self.isVisible()
        geometry = self.geometry()
        self.setWindowFlags(flags)
        self.setGeometry(geometry)
        if was_visible:
            self.show()
        self.preview.setVisible(self.cfg.preview)
        self.capture.configure(self.cfg.fps, self.cfg.quality, self.cfg.max_side)
        self.update_status()

    def _sync_exclude(self) -> None:
        if not self.isVisible():
            self.capture.set_exclude_rect(None)
            return
        rect = self.frameGeometry()
        pad = 14
        self.capture.set_exclude_rect(
            (rect.left() - pad, rect.top() - pad, rect.width() + 2 * pad, rect.height() + 2 * pad)
        )

    def showEvent(self, event) -> None:
        super().showEvent(event)
        self._sync_exclude()

    def hideEvent(self, event) -> None:
        super().hideEvent(event)
        self._sync_exclude()

    def moveEvent(self, event) -> None:
        super().moveEvent(event)
        self._sync_exclude()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._sync_exclude()

    def closeEvent(self, event) -> None:
        if self._quitting or self.tray is None:
            QApplication.quit()
            event.accept()
        else:
            event.ignore()
            self.hide()

    def quit_app(self) -> None:
        self._quitting = True
        self.capture.stop()
        if self.tray:
            self.tray.hide()
        QApplication.quit()

    def toggle_visible(self) -> None:
        self.setVisible(not self.isVisible())
        if self.isVisible():
            self.raise_()
            self.activateWindow()

    def toggle_preview(self) -> None:
        self.cfg.preview = not self.cfg.preview
        self.cfg.save()
        self.preview.setVisible(self.cfg.preview)

    def toggle_pause(self) -> None:
        if self.capture.running:
            self.capture.stop()
            self.add_system("Захват экрана на паузе.")
        else:
            self.capture.start()
            self.add_system("Захват экрана возобновлён.")
        self.pause_btn.setText("▶" if not self.capture.running else "❚❚")
        self.update_status()

    def on_capture_error(self, message: str) -> None:
        self.setToolTip(message)
        if not self._capture_warning_shown:
            self._capture_warning_shown = True
            self.add_system(f"Захват экрана: {message}", error=True)

    # ---------- live frame + status ----------

    def on_frame(self, frame: Frame) -> None:
        image = QImage.fromData(frame.jpeg, b"JPEG")
        if image.isNull():
            return
        pixmap = QPixmap.fromImage(image)
        scaled = pixmap.scaled(self.preview.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation)
        self.preview.setPixmap(scaled)

    def update_status(self) -> None:
        running = self.capture.running
        fps = self.capture.fps if running else 0.0
        self.fps_label.setText(f"{fps:.1f} FPS" if running else "ПАУЗА")
        color = "#6ee7a8" if running else "#e0b45a"
        self.dot.setStyleSheet(f"color:{color}; font-size:11px;")
        self.live_label.setStyleSheet(
            f"background: rgba(10,12,18,190); color:{color}; border-radius:6px;"
            "padding: 2px 6px; font-size: 10px; font-weight: 700;"
        )
        provider = "NIM" if self.cfg.provider == "nvidia" else "OR"
        model = self.cfg.model.split("/")[-1].replace(":free", "")[:20]
        self.provider_label.setText(f"{provider} · {model}" if model else provider)

    # ---------- chat ----------

    def add_system(self, text: str, error: bool = False) -> None:
        bubble = MessageBubble(text, error=error)
        bubble.label.setObjectName("system")
        self._insert_bubble(bubble)

    def add_message(self, text: str, mine: bool = False) -> MessageBubble:
        bubble = MessageBubble(text, mine=mine)
        self._insert_bubble(bubble)
        return bubble

    def _insert_bubble(self, bubble: MessageBubble) -> None:
        self.chat_layout.insertWidget(self.chat_layout.count() - 1, bubble)
        self._bubbles.append(bubble)
        while len(self._bubbles) > 40:
            old = self._bubbles.pop(0)
            self.chat_layout.removeWidget(old)
            old.deleteLater()
        self._scroll_bottom()

    def _scroll_bottom(self) -> None:
        QTimer.singleShot(0, lambda: self.chat.verticalScrollBar().setValue(self.chat.verticalScrollBar().maximum()))

    def append_chunk(self, bubble: MessageBubble, chunk: str) -> None:
        bubble.set_text(bubble.text() + chunk)
        self._scroll_bottom()

    def on_send_clicked(self) -> None:
        if self._worker:
            self._worker.cancel()
            return
        self.send_message()

    def send_message(self) -> None:
        question = self.input.text().strip()
        if not question or self._worker:
            return
        self.input.clear()
        self.add_message(question, mine=True)

        if not self.cfg.api_key:
            self.add_system("Сначала добавь API-ключ OpenRouter или NVIDIA NIM — открою настройки.", error=True)
            self.open_settings()
            return

        frame = self.capture.latest()
        self.history.append({"role": "user", "content": question})
        limit = max(1, self.cfg.history_limit) * 2
        self.history = self.history[-limit:]

        bubble = self.add_message("…")
        self._active_bubble = bubble
        self.set_busy(True)
        worker = LLMWorker(copy.copy(self.cfg), question, frame.jpeg if frame else None, list(self.history[:-1]))
        self._worker = worker
        log_event(f"request start: {self.cfg.provider} / {self.cfg.model}")
        worker.start()

    def _drain_events(self) -> None:
        worker = self._worker
        if worker is None:
            return
        while True:
            try:
                kind, payload = worker.events.get_nowait()
            except queue.Empty:
                return
            if worker is not self._worker or self._active_bubble is None:
                return  # a newer request already took over
            try:
                if kind == "chunk":
                    self.append_chunk(self._active_bubble, payload)
                elif kind == "done":
                    self.on_answer(self._active_bubble, payload)
                    return
                elif kind == "failed":
                    self.on_failure(self._active_bubble, payload)
                    return
                elif kind == "cancelled":
                    self.on_cancelled(self._active_bubble)
                    return
            except Exception:  # never let a UI rendering error kill the app
                log_event("UI error:\n" + traceback.format_exc())
                self._finish_request()
                return

    def set_busy(self, busy: bool) -> None:
        self.input.setDisabled(busy)
        self.send_btn.setText("■" if busy else "➤")
        self.send_btn.setToolTip("Отменить ответ" if busy else "Отправить (Enter)")

    def _finish_request(self) -> None:
        # No QThread/QObject teardown here — that was the source of silent crashes.
        self._worker = None
        self._active_bubble = None
        self.set_busy(False)

    def on_answer(self, bubble: MessageBubble, answer: str) -> None:
        log_event("request ok")
        bubble.set_text(answer)
        self.history.append({"role": "assistant", "content": answer})
        self._finish_request()

    def on_failure(self, bubble: MessageBubble, message: str) -> None:
        log_event(f"request failed: {message}")
        bubble.set_text(message)
        bubble.setObjectName("bubble_error")
        bubble.setStyleSheet("QFrame#bubble_error { background: rgba(190,70,80,45); border-radius:11px; }")
        if self.history and self.history[-1]["role"] == "user":
            self.history.pop()
        self._finish_request()

    def on_cancelled(self, bubble: MessageBubble) -> None:
        log_event("request cancelled")
        if not bubble.text() or bubble.text() == "…":
            bubble.set_text("Отменено.")
        else:
            bubble.set_text(bubble.text() + "\n\n(отменено)")
        if self.history and self.history[-1]["role"] == "user":
            self.history.pop()
        self._finish_request()

    # ---------- settings ----------

    def open_settings(self) -> None:
        dialog = SettingsDialog(self.cfg, self)
        if dialog.exec() == QDialog.Accepted:
            self._apply_flags()
            self.update_status()


class SettingsDialog(QDialog):
    def __init__(self, config: Config, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.cfg = config
        self.setWindowTitle("ScreenMate AI — настройки")
        self.setWindowFlags(self.windowFlags() | Qt.WindowStaysOnTopHint)
        self.resize(540, 610)
        self._loaded: set[str] = set()
        self._build()

    def _build(self) -> None:
        form = QVBoxLayout(self)

        self.provider = QComboBox()
        self.provider.addItem("OpenRouter — бесплатные модели", "openrouter")
        self.provider.addItem("NVIDIA NIM — бесплатные эндпоинты", "nvidia")
        self.provider.setCurrentIndex(1 if self.cfg.provider == "nvidia" else 0)
        form.addWidget(QLabel("Провайдер"))
        form.addWidget(self.provider)

        form.addWidget(QLabel("API-ключ OpenRouter"))
        self.or_key = QLineEdit(self.cfg.openrouter_key)
        self.or_key.setEchoMode(QLineEdit.Password)
        self.or_key.setPlaceholderText("sk-or-v1-…")
        form.addWidget(self.or_key)
        hint_or = QLabel(
            '<a href="https://openrouter.ai/keys" style="color:#9d8cff;">Получить ключ OpenRouter</a>'
        )
        hint_or.setObjectName("hint")
        hint_or.setOpenExternalLinks(True)
        form.addWidget(hint_or)

        form.addWidget(QLabel("API-ключ NVIDIA NIM"))
        self.nv_key = QLineEdit(self.cfg.nvidia_key)
        self.nv_key.setEchoMode(QLineEdit.Password)
        self.nv_key.setPlaceholderText("nvapi-…")
        form.addWidget(self.nv_key)
        hint_nv = QLabel(
            '<a href="https://build.nvidia.com/settings/api-keys" style="color:#9d8cff;">Получить ключ NVIDIA NIM</a>'
        )
        hint_nv.setObjectName("hint")
        hint_nv.setOpenExternalLinks(True)
        form.addWidget(hint_nv)

        self.show_keys = QCheckBox("Показать ключи")
        self.show_keys.stateChanged.connect(self._toggle_keys)
        form.addWidget(self.show_keys)

        form.addWidget(QLabel("Модель OpenRouter (с поддержкой изображений)"))
        self.or_model = QComboBox()
        self.or_model.setEditable(True)
        form.addWidget(self._model_row(self.or_model, "openrouter"))

        form.addWidget(QLabel("Модель NVIDIA NIM"))
        self.nv_model = QComboBox()
        self.nv_model.setEditable(True)
        form.addWidget(self._model_row(self.nv_model, "nvidia"))

        self.model_status = QLabel("")
        self.model_status.setObjectName("status")
        form.addWidget(self.model_status)

        grid = QHBoxLayout()
        grid.addWidget(QLabel("FPS захвата"))
        self.fps = QSpinBox()
        self.fps.setRange(1, 15)
        self.fps.setValue(self.cfg.fps)
        self.fps.setSuffix(" fps")
        grid.addWidget(self.fps)
        grid.addWidget(QLabel("Качество JPEG"))
        self.quality = QSpinBox()
        self.quality.setRange(35, 95)
        self.quality.setValue(self.cfg.quality)
        grid.addWidget(self.quality)
        grid.addStretch(1)
        form.addLayout(grid)

        self.preview_cb = QCheckBox("Показывать превью экрана в чате")
        self.preview_cb.setChecked(self.cfg.preview)
        self.ontop_cb = QCheckBox("Чат поверх всех окон")
        self.ontop_cb.setChecked(self.cfg.always_on_top)
        self.attach_cb = QCheckBox("Прикреплять последний кадр к каждому вопросу")
        self.attach_cb.setChecked(self.cfg.attach_screenshot)
        form.addWidget(self.preview_cb)
        form.addWidget(self.ontop_cb)
        form.addWidget(self.attach_cb)

        form.addWidget(QLabel("Системная инструкция"))
        self.prompt = QPlainTextEdit(self.cfg.system_prompt)
        form.addWidget(self.prompt)

        buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addWidget(buttons)

        self.provider.currentIndexChanged.connect(lambda: self._load_models(self.provider.currentData()))
        self.or_model.setEditText(self.cfg.openrouter_model)
        self.nv_model.setEditText(self.cfg.nvidia_model)
        if self.cfg.provider == "nvidia":
            self._load_models("nvidia", initial=self.cfg.nvidia_model)
        else:
            self._load_models("openrouter", initial=self.cfg.openrouter_model)

    def _model_row(self, combo: QComboBox, provider: str) -> QWidget:
        row = QWidget()
        layout = QHBoxLayout(row)
        layout.setContentsMargins(0, 0, 0, 0)
        refresh = QPushButton("обновить")
        refresh.setObjectName("small")
        refresh.clicked.connect(lambda: self._load_models(provider, force=True))
        layout.addWidget(combo, 1)
        layout.addWidget(refresh)
        return row

    def _toggle_keys(self, state: int) -> None:
        mode = QLineEdit.Normal if state else QLineEdit.Password
        self.or_key.setEchoMode(mode)
        self.nv_key.setEchoMode(mode)

    def _load_models(self, provider: str, force: bool = False, initial: str = "") -> None:
        if provider not in {"openrouter", "nvidia"}:
            return
        if provider in self._loaded and not force:
            return
        self._loaded.add(provider)
        combo = self.or_model if provider == "openrouter" else self.nv_model
        key = self.or_key.text().strip() if provider == "openrouter" else self.nv_key.text().strip()
        current = initial or combo.currentText().strip()
        self.model_status.setText("Загружаю список моделей…")
        models: list[ModelInfo] = list_models(provider, key, force=force)
        combo.clear()
        for model in models:
            label = model.label if model.label != model.id else model.id
            combo.addItem(label, model.id)
        idx = combo.findData(current)
        if idx >= 0:
            combo.setCurrentIndex(idx)
        elif current:
            combo.setEditText(current)
        elif combo.count():
            combo.setCurrentIndex(0)
        self.model_status.setText(
            f"Загружено моделей: {len(models)}. Можно ввести свою модель вручную."
        )

    def accept(self) -> None:  # noqa: N802 - Qt naming
        self.cfg.provider = self.provider.currentData() or "openrouter"
        self.cfg.openrouter_key = self.or_key.text().strip()
        self.cfg.nvidia_key = self.nv_key.text().strip()
        self.cfg.openrouter_model = (self.or_model.currentData() or self.or_model.currentText()).strip()
        self.cfg.nvidia_model = (self.nv_model.currentData() or self.nv_model.currentText()).strip()
        self.cfg.preview = self.preview_cb.isChecked()
        self.cfg.always_on_top = self.ontop_cb.isChecked()
        self.cfg.attach_screenshot = self.attach_cb.isChecked()
        self.cfg.fps = self.fps.value()
        self.cfg.quality = self.quality.value()
        self.cfg.system_prompt = self.prompt.toPlainText().strip()
        self.cfg.save()
        super().accept()
