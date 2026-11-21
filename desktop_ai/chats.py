"""Persistent chat sessions, stored as small JSON files next to config.json."""

from __future__ import annotations

import json
import re
import uuid
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Optional

from .config import CONFIG_FILE

CHATS_DIR = CONFIG_FILE.parent / "chats"
ACTIVE_FILE = CHATS_DIR / "active.json"


def _now() -> str:
    return datetime.now().isoformat(timespec="seconds")


@dataclass
class Chat:
    id: str
    title: str = "Новый чат"
    created_at: str = field(default_factory=_now)
    updated_at: str = field(default_factory=_now)
    messages: list[dict[str, str]] = field(default_factory=list)


class ChatStore:
    def __init__(self) -> None:
        CHATS_DIR.mkdir(parents=True, exist_ok=True)

    def _path(self, chat_id: str) -> Path:
        if not re.fullmatch(r"[a-f0-9]{12}", chat_id):
            raise ValueError("bad chat id")
        return CHATS_DIR / f"{chat_id}.json"

    def list(self) -> list[Chat]:
        chats: list[Chat] = []
        for path in CHATS_DIR.glob("*.json"):
            if path.name == "active.json":
                continue
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
                chats.append(
                    Chat(
                        id=str(data["id"]),
                        title=str(data.get("title") or "Новый чат"),
                        created_at=str(data.get("created_at") or _now()),
                        updated_at=str(data.get("updated_at") or _now()),
                        messages=[
                            {
                                "role": str(m.get("role", "user")),
                                "content": str(m.get("content", "")),
                            }
                            for m in data.get("messages", [])
                            if isinstance(m, dict)
                        ],
                    )
                )
            except (OSError, ValueError, KeyError):
                continue
        chats.sort(key=lambda c: c.updated_at, reverse=True)
        return chats

    def get(self, chat_id: str) -> Optional[Chat]:
        try:
            data = json.loads(self._path(chat_id).read_text(encoding="utf-8"))
            return Chat(
                id=str(data["id"]),
                title=str(data.get("title") or "Новый чат"),
                created_at=str(data.get("created_at") or _now()),
                updated_at=str(data.get("updated_at") or _now()),
                messages=[dict(m) for m in data.get("messages", [])],
            )
        except (OSError, ValueError, KeyError):
            return None

    def save(self, chat: Chat) -> None:
        chat.updated_at = _now()
        first_user = next((m["content"] for m in chat.messages if m.get("role") == "user"), "")
        if first_user:
            title = first_user.replace("\n", " ").strip()
            chat.title = title[:38] + ("…" if len(title) > 38 else "")
        else:
            chat.title = "Новый чат"
        path = self._path(chat.id)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(asdict(chat), ensure_ascii=False, indent=2), encoding="utf-8")
        tmp.replace(path)

    def create(self) -> Chat:
        chat = Chat(id=uuid.uuid4().hex[:12])
        self.save(chat)
        self.set_active(chat.id)
        return chat

    def delete(self, chat_id: str) -> None:
        try:
            self._path(chat_id).unlink(missing_ok=True)
        except (OSError, ValueError):
            pass

    def active_id(self) -> str:
        try:
            return str(json.loads(ACTIVE_FILE.read_text(encoding="utf-8")).get("id", ""))
        except (OSError, ValueError):
            return ""

    def set_active(self, chat_id: str) -> None:
        try:
            ACTIVE_FILE.write_text(json.dumps({"id": chat_id}), encoding="utf-8")
        except OSError:
            pass
