"""Fast unit tests for the parts that do not need a display."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from desktop_ai import config as config_module
from desktop_ai.config import Config
from desktop_ai.providers import ProviderError, ask_stream, build_messages, list_models


class FakeResponse:
    status_code = 200
    text = ""

    def iter_lines(self, decode_unicode=True):
        yield 'data: {"choices":[{"delta":{"content":"Привет"}}]}'
        yield ""
        yield 'data: {"choices":[{"delta":{"content":", мир!"}}]}'
        yield "data: [DONE]"

    def close(self):
        pass


class ConfigTests(unittest.TestCase):
    def test_env_keys_and_clamping(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "config.json"
            with patch.object(config_module, "CONFIG_FILE", cfg_path), patch.dict(
                os.environ,
                {"OPENROUTER_API_KEY": "sk-test", "NVIDIA_NIM_API_KEY": "nv-test"},
                clear=True,
            ):
                cfg = Config.load()
                self.assertEqual(cfg.api_key, "sk-test")
                self.assertEqual(cfg.fps, 5)
                cfg.provider = "nvidia"
                self.assertEqual(cfg.api_key, "nv-test")
                cfg.fps = 999
                cfg.normalize()
                self.assertEqual(cfg.fps, 15)

    def test_save_and_reload(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "config.json"
            with patch.object(config_module, "CONFIG_FILE", cfg_path), patch.dict(os.environ, {}, clear=True):
                cfg = Config()
                cfg.openrouter_key = "sk-saved"
                cfg.save()
                self.assertIn("sk-saved", json.loads(cfg_path.read_text(encoding="utf-8"))["openrouter_key"])
                self.assertEqual(Config.load().openrouter_key, "sk-saved")


class ProviderTests(unittest.TestCase):
    def test_build_messages_attaches_screenshot(self):
        messages = build_messages(
            "Что здесь написано?",
            "sys",
            [{"role": "user", "content": "привет"}, {"role": "assistant", "content": "здравствуй"}],
            b"jpegbytes",
        )
        self.assertEqual(messages[0]["role"], "system")
        self.assertEqual(len(messages), 4)
        user = messages[-1]["content"]
        self.assertEqual(user[0]["text"], "Что здесь написано?")
        self.assertTrue(user[1]["image_url"]["url"].startswith("data:image/jpeg;base64,"))

    def test_build_messages_without_screenshot(self):
        messages = build_messages("текст", "sys", [], b"jpegbytes", attach_screenshot=False)
        self.assertEqual(messages[-1]["content"], [{"type": "text", "text": "текст"}])

    def test_streaming_answer(self):
        cfg = SimpleNamespace(
            provider="openrouter",
            api_key="sk-test",
            model="model:free",
            system_prompt="sys",
            attach_screenshot=True,
        )
        chunks = []
        with patch("desktop_ai.providers.requests.post", return_value=FakeResponse()) as post:
            answer = ask_stream(cfg, "вопрос", b"jpeg", on_chunk=chunks.append)
        self.assertEqual(answer, "Привет, мир!")
        self.assertEqual(chunks, ["Привет", ", мир!"])
        self.assertTrue(post.call_args.kwargs["json"]["stream"])

    def test_missing_key_error(self):
        cfg = SimpleNamespace(provider="openrouter", api_key="", model="model:free")
        with self.assertRaises(ProviderError):
            ask_stream(cfg, "вопрос", None)

    def test_http_error_message(self):
        response = SimpleNamespace(
            status_code=401,
            text='{"error":{"message":"invalid key"}}',
            close=lambda: None,
        )
        cfg = SimpleNamespace(provider="nvidia", api_key="bad", model="m", system_prompt="s")
        with patch("desktop_ai.providers.requests.post", return_value=response):
            with self.assertRaisesRegex(ProviderError, "Неверный API-ключ"):
                ask_stream(cfg, "вопрос", None)

    def test_model_catalogue_fallback(self):
        with patch("desktop_ai.providers.requests.get", side_effect=OSError("offline")):
            models = list_models("openrouter", force=True)
        self.assertTrue(models)
        self.assertTrue(any(m.id.endswith(":free") for m in models))


if __name__ == "__main__":
    unittest.main()
