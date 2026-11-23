"""Settings storage for ScreenMate AI.

Keys can come from three places (in priority order):
1. settings saved through the in-app settings dialog (config.json),
2. the .env file next to run.bat,
3. real environment variables.
"""

from __future__ import annotations

import json
import os
import stat
import tempfile
from dataclasses import asdict, dataclass, fields
from pathlib import Path
from typing import Any

APP_NAME = "ScreenMate AI"
ROOT_DIR = Path(__file__).resolve().parent.parent
ENV_FILE = ROOT_DIR / ".env"


def _data_dir() -> Path:
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA") or (Path.home() / "AppData" / "Roaming"))
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    path = base / "screenmate_ai"
    path.mkdir(parents=True, exist_ok=True)
    return path


CONFIG_FILE = _data_dir() / "config.json"

DEFAULT_SYSTEM_PROMPT = (
    "Ты — ScreenMate AI, невидимый помощник, который видит экран пользователя. "
    "Отвечай на русском языке, коротко и по делу. Используй текст, который видно на "
    "скриншоте: кнопки, меню, ошибки, код, письма. Если чего-то не видно или ответ "
    "неточный — прямо скажи об этом. Не выдумывай содержимое экрана."
)

DEFAULTS: dict[str, Any] = {
    "provider": "openrouter",
    "openrouter_key": "",
    "nvidia_key": "",
    "openrouter_model": "google/gemma-4-31b-it:free",
    "nvidia_model": "nvidia/nemotron-3.5-super-vl-120b-a12b",
    "fps": 5,
    "quality": 70,
    "max_side": 1600,
    "history_limit": 8,
    "preview": True,
    "always_on_top": True,
    "attach_screenshot": True,
    "allow_control": True,
    "region": [],
    "system_prompt": DEFAULT_SYSTEM_PROMPT,
}


def read_env(path: Path = ENV_FILE) -> dict[str, str]:
    """Minimal .env parser — no python-dotenv dependency needed."""
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8-sig").splitlines()
    except OSError:
        return values
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].strip()
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[key.strip()] = value
    return values


@dataclass
class Config:
    provider: str = "openrouter"
    openrouter_key: str = ""
    nvidia_key: str = ""
    openrouter_model: str = DEFAULTS["openrouter_model"]
    nvidia_model: str = DEFAULTS["nvidia_model"]
    fps: int = 5
    quality: int = 70
    max_side: int = 1600
    history_limit: int = 8
    preview: bool = True
    always_on_top: bool = True
    attach_screenshot: bool = True
    allow_control: bool = True
    region: list[int] = None  # type: ignore[assignment]
    system_prompt: str = DEFAULT_SYSTEM_PROMPT

    @classmethod
    def load(cls) -> "Config":
        data: dict[str, Any] = dict(DEFAULTS)
        try:
            saved = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
            if isinstance(saved, dict):
                data.update({k: v for k, v in saved.items() if k in DEFAULTS})
        except (OSError, ValueError):
            pass

        env = read_env()
        env_map = {
            "openrouter_key": ("OPENROUTER_API_KEY",),
            "nvidia_key": ("NVIDIA_NIM_API_KEY", "NVIDIA_API_KEY"),
            "openrouter_model": ("OPENROUTER_MODEL",),
            "nvidia_model": ("NVIDIA_MODEL",),
        }
        for field_name, names in env_map.items():
            for name in names:
                value = os.environ.get(name) or env.get(name)
                if value:
                    data[field_name] = value
                    break

        valid = {f.name for f in fields(cls)}
        cfg = cls(**{k: v for k, v in data.items() if k in valid})
        cfg.normalize()
        return cfg

    def normalize(self) -> None:
        self.provider = "nvidia" if str(self.provider).lower() in {"nvidia", "nim", "nvidia_nim"} else "openrouter"
        self.allow_control = bool(self.allow_control)
        self.fps = _clamp_int(self.fps, 1, 15, 5)
        self.quality = _clamp_int(self.quality, 35, 95, 70)
        self.max_side = _clamp_int(self.max_side, 640, 2560, 1600)
        self.history_limit = _clamp_int(self.history_limit, 0, 20, 8)
        for name in ("preview", "always_on_top", "attach_screenshot"):
            setattr(self, name, bool(getattr(self, name)))
        for name in ("openrouter_key", "nvidia_key", "openrouter_model", "nvidia_model"):
            setattr(self, name, str(getattr(self, name) or "").strip())
        region = getattr(self, "region", None)
        if not isinstance(region, (list, tuple)) or len(region) != 4:
            region = []
        else:
            try:
                region = [int(v) for v in region]
                if region[2] < 120 or region[3] < 80:
                    region = []
            except (TypeError, ValueError):
                region = []
        self.region = region
        if not self.system_prompt or not str(self.system_prompt).strip():
            self.system_prompt = DEFAULT_SYSTEM_PROMPT

    @property
    def api_key(self) -> str:
        return self.nvidia_key if self.provider == "nvidia" else self.openrouter_key

    @property
    def model(self) -> str:
        return self.nvidia_model if self.provider == "nvidia" else self.openrouter_model

    def save(self) -> None:
        self.normalize()
        payload = json.dumps(asdict(self), ensure_ascii=False, indent=2)
        fd, tmp_name = tempfile.mkstemp(prefix="config-", suffix=".json", dir=str(CONFIG_FILE.parent))
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                fh.write(payload)
            os.replace(tmp_name, CONFIG_FILE)
            try:
                CONFIG_FILE.chmod(stat.S_IRUSR | stat.S_IWUSR)
            except OSError:
                pass
        finally:
            if os.path.exists(tmp_name):
                os.unlink(tmp_name)


def _clamp_int(value: Any, low: int, high: int, default: int) -> int:
    try:
        return max(low, min(high, int(value)))
    except (TypeError, ValueError):
        return default
