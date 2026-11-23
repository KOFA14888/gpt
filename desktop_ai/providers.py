"""OpenAI-compatible API clients for OpenRouter and NVIDIA NIM."""

from __future__ import annotations

import base64
import json
import re
import time
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Optional

import requests

OPENROUTER_CHAT = "https://openrouter.ai/api/v1/chat/completions"
OPENROUTER_MODELS = "https://openrouter.ai/api/v1/models"
NVIDIA_CHAT = "https://integrate.api.nvidia.com/v1/chat/completions"
NVIDIA_MODELS = "https://integrate.api.nvidia.com/v1/models"

# Used only if the live model catalogue cannot be reached.
FALLBACK_MODELS: dict[str, tuple[str, ...]] = {
    "openrouter": (
        "google/gemma-4-31b-it:free",
        "google/gemma-4-26b-a4b-it:free",
        "nvidia/nemotron-nano-12b-v2-vl:free",
        "nvidia/nemotron-3.5-super-vl:free",
        "openrouter/free",
    ),
    "nvidia": (
        "nvidia/nemotron-3.5-super-vl-120b-a12b",
        "meta/llama-3.2-11b-vision-instruct",
        "meta/llama-3.2-90b-vision-instruct",
        "nvidia/nemotron-nano-12b-v2-vl",
        "google/gemma-4-31b-it",
    ),
}

_MODEL_CACHE: dict[str, tuple[float, list["ModelInfo"]]] = {}
_CACHE_TTL = 15 * 60


class ProviderError(RuntimeError):
    """User-facing API error."""


class CancelledError(ProviderError):
    pass


@dataclass(frozen=True)
class ModelInfo:
    id: str
    label: str
    context: int = 0

    def __str__(self) -> str:
        return self.id


def _http_error(status: int, body: str) -> str:
    message = ""
    try:
        payload = json.loads(body)
        error = payload.get("error", payload)
        if isinstance(error, dict):
            message = str(error.get("message") or error.get("detail") or "")
    except (ValueError, AttributeError):
        message = body.strip()[:200]
    prefix = {
        401: "Неверный API-ключ (401).",
        402: "Нужны кредиты или ключ не имеет доступа (402).",
        403: "Доступ запрещён (403).",
        404: "Модель не найдена или больше не доступна (404). Обнови список моделей.",
        408: "Превышено время ожидания запроса (408).",
        429: "Лимит запросов/бесплатной модели исчерпан (429). Попробуй другую модель или позже.",
        500: "Ошибка на стороне провайдера (500).",
        502: "Плохой ответ провайдера (502).",
        503: "Сервис временно перегружен (503).",
    }.get(status, f"Ошибка API ({status}).")
    return f"{prefix} {message}".strip()


def list_models(provider: str, api_key: str = "", force: bool = False) -> list[ModelInfo]:
    """Fetch vision-capable models; free ones first for OpenRouter."""
    provider = "nvidia" if provider == "nvidia" else "openrouter"
    key = f"{provider}:{api_key[-8:]}"
    cached = _MODEL_CACHE.get(key)
    if cached and not force and time.time() - cached[0] < _CACHE_TTL:
        return cached[1]

    try:
        if provider == "openrouter":
            models = _fetch_openrouter_models()
        else:
            models = _fetch_nvidia_models(api_key)
        if models:
            _MODEL_CACHE[key] = (time.time(), models)
            return models
    except (requests.RequestException, ValueError, OSError):
        pass
    return [ModelInfo(mid, mid) for mid in FALLBACK_MODELS[provider]]


def _fetch_openrouter_models() -> list[ModelInfo]:
    response = requests.get(OPENROUTER_MODELS, timeout=(8, 20))
    response.raise_for_status()
    payload = response.json()
    result: list[ModelInfo] = []
    for item in payload.get("data", []):
        mid = str(item.get("id", ""))
        arch = item.get("architecture") or {}
        if "image" not in (arch.get("input_modalities") or []):
            continue
        pricing = item.get("pricing") or {}
        try:
            free = ":free" in mid or (
                float(pricing.get("prompt", "1")) == 0 and float(pricing.get("completion", "1")) == 0
            )
        except (TypeError, ValueError):
            free = ":free" in mid
        if not free:
            continue
        result.append(ModelInfo(mid, str(item.get("name") or mid), int(item.get("context_length") or 0)))
    result.sort(key=lambda m: ((":" not in m.id), m.id))
    return result


_NVIDIA_VISION = re.compile(
    r"(vl|vision|omni|multimodal|gemma|maverick|cosmos|super-vl)", re.IGNORECASE
)
_NVIDIA_EXCLUDE = re.compile(r"(embed|guard|safety|retriev|transcrib|ocr|image-edit)", re.IGNORECASE)


def _fetch_nvidia_models(api_key: str) -> list[ModelInfo]:
    headers = {"Authorization": f"Bearer {api_key}"} if api_key else {}
    response = requests.get(NVIDIA_MODELS, headers=headers, timeout=(8, 20))
    response.raise_for_status()
    payload = response.json()
    result: list[ModelInfo] = []
    for item in payload.get("data", []):
        mid = str(item.get("id", ""))
        if not mid or _NVIDIA_EXCLUDE.search(mid) or not _NVIDIA_VISION.search(mid):
            continue
        result.append(ModelInfo(mid, str(item.get("name") or mid), int(item.get("context_length") or 0)))
    result.sort(key=lambda m: m.id)
    return result


def build_messages(
    question: str,
    system_prompt: str,
    history: Iterable[dict[str, str]],
    frame_jpeg: Optional[bytes] = None,
    attach_screenshot: bool = True,
) -> list[dict[str, Any]]:
    messages: list[dict[str, Any]] = [{"role": "system", "content": system_prompt}]
    for item in history:
        role = item.get("role")
        text = str(item.get("content") or "")
        if role in {"user", "assistant"} and text:
            messages.append({"role": role, "content": text})

    parts: list[dict[str, Any]] = [{"type": "text", "text": question}]
    if attach_screenshot and frame_jpeg:
        encoded = base64.b64encode(frame_jpeg).decode("ascii")
        parts.append(
            {
                "type": "image_url",
                "image_url": {"url": f"data:image/jpeg;base64,{encoded}", "detail": "high"},
            }
        )
    messages.append({"role": "user", "content": parts})
    return messages


def _chat_headers(provider: str, api_key: str) -> dict[str, str]:
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
        "Accept": "application/json",
    }
    if provider == "openrouter":
        headers["HTTP-Referer"] = "https://github.com/KOFA14888/gpt"
        headers["X-Title"] = "ScreenMate AI"
    return headers


def ask_stream(
    config: Any,
    question: str,
    frame_jpeg: Optional[bytes],
    history: Iterable[dict[str, str]] = (),
    on_chunk: Optional[Callable[[str], None]] = None,
    cancelled: Optional[Callable[[], bool]] = None,
) -> str:
    """Stream an answer. Raises ProviderError with a readable Russian message."""
    provider = "nvidia" if getattr(config, "provider", "") == "nvidia" else "openrouter"
    api_key = getattr(config, "api_key", "") or ""
    model = getattr(config, "model", "") or ""
    if not api_key:
        raise ProviderError(
            "Нет API-ключа. Открой настройки (шестерёнка) и вставь ключ OpenRouter или NVIDIA NIM."
        )
    if not model:
        raise ProviderError("Не выбрана модель. Открой настройки и выбери модель со зрением.")

    payload = {
        "model": model,
        "messages": build_messages(
            question=question,
            system_prompt=getattr(config, "system_prompt", "Отвечай по-русски."),
            history=history,
            frame_jpeg=frame_jpeg,
            attach_screenshot=bool(getattr(config, "attach_screenshot", True)),
        ),
        "stream": True,
        "temperature": 0.3,
        "max_tokens": 1024,
    }
    url = NVIDIA_CHAT if provider == "nvidia" else OPENROUTER_CHAT
    answer_parts: list[str] = []

    response = requests.post(
        url,
        headers=_chat_headers(provider, api_key),
        json=payload,
        stream=True,
        # Free/vision models can be very slow to emit the first token;
        # a short read timeout was killing answers with an empty response.
        timeout=(30, 600),
    )
    # SSE is UTF-8 by spec. requests may otherwise decode it as Latin-1 when
    # the provider omits charset — that produces "Ð" instead of Cyrillic.
    response.encoding = "utf-8"
    if response.status_code != 200:
        body = response.text[:2000]
        response.close()
        raise ProviderError(_http_error(response.status_code, body))

    try:
        for raw_line in response.iter_lines(decode_unicode=False):
            if isinstance(raw_line, bytes):
                line = raw_line.decode("utf-8", "replace")
            else:
                line = raw_line
            if cancelled and cancelled():
                raise CancelledError("Ответ отменён.")
            if not line or not line.startswith("data:"):
                continue
            data = line[5:].strip()
            if data == "[DONE]":
                break
            try:
                event = json.loads(data)
            except ValueError:
                continue
            if isinstance(event, dict) and event.get("error"):
                error = event["error"]
                raise ProviderError(_http_error(429, json.dumps(error)) if isinstance(error, dict) else str(error))
            choices = (event.get("choices") or []) if isinstance(event, dict) else []
            if not choices:
                continue
            delta = choices[0].get("delta") or {}
            chunk = delta.get("content") if isinstance(delta, dict) else None
            if isinstance(chunk, list):  # some providers emit content parts
                chunk = "".join(str(part.get("text", "")) for part in chunk if isinstance(part, dict))
            if isinstance(chunk, str) and chunk:
                answer_parts.append(chunk)
                if on_chunk:
                    on_chunk(chunk)
    except requests.exceptions.Timeout as exc:
        # Keep whatever already streamed instead of discarding a long answer.
        partial = "".join(answer_parts).strip()
        if partial:
            return partial
        raise ProviderError(
            "Модель не ответила за отведённое время (таймаут). "
            "Попробуй ещё раз или выбери другую модель."
        ) from exc
    finally:
        response.close()

    answer = "".join(answer_parts).strip()
    if not answer:
        raise ProviderError(
            "Модель вернула пустой ответ. Попробуй другую модель или повтори запрос."
        )
    return answer
