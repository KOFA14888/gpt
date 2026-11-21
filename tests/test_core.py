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

    def test_utf8_sse_decoded_correctly(self):
        cfg = SimpleNamespace(
            provider="openrouter", api_key="sk-test", model="m:free",
            system_prompt="sys", attach_screenshot=True,
        )
        with patch("desktop_ai.providers.requests.post", return_value=Utf8SseResponse()):
            answer = ask_stream(cfg, "вопрос", None)
        self.assertEqual(answer, "Я вижу экран")

    def test_model_catalogue_fallback(self):
        with patch("desktop_ai.providers.requests.get", side_effect=OSError("offline")):
            models = list_models("openrouter", force=True)
        self.assertTrue(models)
        self.assertTrue(any(m.id.endswith(":free") for m in models))




class Utf8SseResponse(FakeResponse):
    def iter_lines(self, decode_unicode=True):
        # Simulates a provider that sends raw UTF-8 without a charset header.
        yield 'data: {"choices":[{"delta":{"content":"Я вижу экран"}}]}'.encode("utf-8")
        yield b"data: [DONE]"


class WorkerTests(unittest.TestCase):
    def _collect(self, worker):
        events = []
        worker.start()
        worker.thread.join(timeout=3)
        while True:
            try:
                events.append(worker.events.get_nowait())
            except Exception:
                break
        return events

    def test_worker_streams_then_finishes(self):
        def fake_ask(**kwargs):
            kwargs["on_chunk"]("Привет")
            kwargs["on_chunk"](", мир!")
            return "Привет, мир!"

        with patch("desktop_ai.worker.ask_stream", side_effect=fake_ask):
            from desktop_ai.worker import LLMWorker

            events = self._collect(LLMWorker(object(), "вопрос", None, []))
        self.assertEqual(events, [("chunk", "Привет"), ("chunk", ", мир!"), ("done", "Привет, мир!")])

    def test_worker_reports_failure_without_crashing(self):
        from desktop_ai.worker import LLMWorker

        with patch("desktop_ai.worker.ask_stream", side_effect=RuntimeError("лимит исчерпан")):
            events = self._collect(LLMWorker(object(), "question", None, []))
        self.assertEqual(events, [("failed", "лимит исчерпан")])


class ActionTests(unittest.TestCase):
    def test_parse_commands(self):
        from desktop_ai.actions import parse_commands

        text = "Сдвигаюсь.\n[MOVE 120 340]\n[CLICK 120 340]\nГотово."
        clean, actions = parse_commands(text)
        self.assertEqual(clean, "Сдвигаюсь.\n\nГотово.")
        self.assertEqual([a.kind for a in actions], ["MOVE", "CLICK"])
        self.assertEqual((actions[0].x, actions[0].y), (120, 340))

    def test_stream_preview_hides_partial_command(self):
        from desktop_ai.actions import clean_stream_preview

        self.assertEqual(clean_stream_preview("Двигаю [MOV"), "Двигаю")
        self.assertEqual(clean_stream_preview("Привет [CLICK 10 20] мир"), "Привет мир")


class ChatStoreTests(unittest.TestCase):
    def test_create_save_switch_delete(self):
        import tempfile

        from desktop_ai import chats as chats_module

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with patch.object(chats_module, "CHATS_DIR", root / "chats"), patch.object(
                chats_module, "ACTIVE_FILE", root / "chats" / "active.json"
            ):
                store = chats_module.ChatStore()
                chat = store.create()
                chat.messages = [{"role": "user", "content": "Что на экране?"}]
                store.save(chat)
                self.assertEqual(store.active_id(), chat.id)
                self.assertEqual(store.list()[0].title, "Что на экране?")
                self.assertEqual(store.get(chat.id).messages[0]["content"], "Что на экране?")
                store.delete(chat.id)
                self.assertEqual(store.list(), [])


if __name__ == "__main__":
    unittest.main()
