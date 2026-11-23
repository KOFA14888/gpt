// Graphical UI: embeds a polished HTML chat and serves it on 127.0.0.1.
// Streams agent output over SSE so the frontend can animate tokens live.

#include "webui.hpp"

#include "agent.hpp"
#include "common.hpp"
#include "config.hpp"
#include "tools.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// sockets from common.hpp (winsock2 before windows.h)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

namespace sm {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Embedded frontend
// ---------------------------------------------------------------------------

static const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>screenmate — coding agent</title>
<style>
  :root {
    --bg: #0a0a0a;
    --bg-soft: #111111;
    --surface: #161616;
    --surface2: #1d1d1d;
    --border: #262626;
    --border-strong: #383838;
    --text: #e5e5e5;
    --muted: #8b8b8b;
    --dim: #5c5c5c;
    --accent: #fab283;
    --accent-dim: #c4713a;
    --purple: #9d7cd8;
    --green: #7fd88f;
    --red: #e06c75;
    --sans: -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif;
    --mono: "SF Mono", ui-monospace, "JetBrains Mono", Menlo, Consolas, "Cascadia Code", monospace;
    --radius: 6px;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body { height: 100%; }
  body {
    background: var(--bg);
    color: var(--text);
    font: 14.5px/1.55 var(--sans);
    -webkit-font-smoothing: antialiased;
    overflow: hidden;
  }
  .app {
    height: 100%;
    display: flex;
    flex-direction: column;
  }

  /* ---------- header ---------- */
  header {
    flex: none;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 0 20px;
    height: 50px;
    border-bottom: 1px solid var(--border);
    background: var(--bg);
  }
  .brand { display: flex; align-items: baseline; gap: 12px; min-width: 0; }
  .brand h1 {
    font: 600 14px var(--mono);
    letter-spacing: .02em;
    color: var(--text);
    white-space: nowrap;
  }
  .brand h1 .cursor {
    display: inline-block;
    width: 7px; height: 13px;
    margin-left: 5px;
    background: var(--accent);
    vertical-align: -2px;
    animation: blink 1.2s steps(1) infinite;
  }
  @keyframes blink { 50% { opacity: 0; } }
  .brand .tag {
    font: 12px var(--mono);
    color: var(--dim);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .headright { display: flex; align-items: center; gap: 10px; }

  .mode {
    display: flex;
    border: 1px solid var(--border);
    border-radius: var(--radius);
    background: var(--bg-soft);
    overflow: hidden;
  }
  .mode button {
    background: transparent;
    border: 0;
    color: var(--muted);
    font: 500 12px var(--mono);
    letter-spacing: .04em;
    padding: 6px 14px;
    cursor: pointer;
    transition: color .15s ease, background .15s ease;
  }
  .mode button:hover { color: var(--text); }
  .mode button.active { background: var(--surface2); }
  .mode button[data-mode="build"].active { color: var(--accent); box-shadow: inset 0 -2px 0 var(--accent); }
  .mode button[data-mode="plan"].active { color: var(--green); box-shadow: inset 0 -2px 0 var(--green); }

  .iconbtn {
    width: 30px; height: 30px;
    display: grid; place-items: center;
    background: transparent;
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--muted);
    cursor: pointer;
    transition: color .15s ease, border-color .15s ease;
  }
  .iconbtn:hover { color: var(--text); border-color: var(--border-strong); }
  .iconbtn svg { width: 15px; height: 15px; }

  /* ---------- chat ---------- */
  .chat {
    flex: 1;
    overflow-y: auto;
    scroll-behavior: smooth;
  }
  .chat-inner, .hero, .banner, #stream {
    max-width: 760px;
    margin-left: auto;
    margin-right: auto;
  }
  .chat { padding: 0 20px; }
  .chat::-webkit-scrollbar { width: 10px; }
  .chat::-webkit-scrollbar-thumb { background: #222; border: 3px solid var(--bg); border-radius: 6px; }
  .chat::-webkit-scrollbar-thumb:hover { background: #2e2e2e; }

  .msg {
    max-width: 760px;
    margin: 16px auto;
    font-size: 14.5px;
    line-height: 1.6;
    word-break: break-word;
    animation: fadeIn .18s ease both;
  }
  @keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }
  .msg .meta {
    font: 11px var(--mono);
    text-transform: uppercase;
    letter-spacing: .07em;
    color: var(--dim);
    margin-bottom: 5px;
  }
  .msg.user {
    max-width: 82%;
    margin-left: auto;
    margin-right: auto;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 10px 14px;
  }
  .msg.user .meta { color: var(--accent-dim); }
  .msg.ai .meta { color: var(--purple); }
  .msg.ai .body { color: var(--text); }
  .msg.sys {
    text-align: center;
    font: 12px var(--mono);
    color: var(--muted);
  }
  .msg.err {
    background: rgba(224, 108, 117, .07);
    border: 1px solid rgba(224, 108, 117, .3);
    border-radius: var(--radius);
    padding: 9px 13px;
    font: 13px var(--mono);
    color: var(--red);
    max-width: 760px;
  }
  .msg img.shot {
    display: block;
    max-width: 100%;
    margin-top: 10px;
    border: 1px solid var(--border);
    border-radius: 4px;
  }

  /* status line */
  .status {
    max-width: 760px;
    margin: 0 auto;
    min-height: 20px;
    padding-top: 10px;
    font: 12px var(--mono);
    color: var(--accent);
    opacity: 0;
    transition: opacity .2s ease;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .status.on { opacity: 1; }
  .status .dot {
    display: inline-block;
    width: 6px; height: 6px;
    background: var(--accent);
    margin-right: 8px;
    animation: blink 1s steps(1) infinite;
  }

  /* typing */
  .typing { display: inline-flex; gap: 5px; padding: 4px 0; }
  .typing i {
    width: 5px; height: 5px;
    background: var(--dim);
    animation: typePulse 1.1s ease-in-out infinite;
  }
  .typing i:nth-child(2) { animation-delay: .18s; }
  .typing i:nth-child(3) { animation-delay: .36s; }
  @keyframes typePulse { 0%, 100% { opacity: .3; } 50% { opacity: 1; background: var(--muted); } }

  /* hero */
  .hero { padding: 56px 0 8px; }
  .hero h2 {
    font-size: 21px;
    font-weight: 650;
    letter-spacing: -.01em;
    margin-bottom: 8px;
  }
  .hero p {
    color: var(--muted);
    font-size: 14px;
    max-width: 540px;
    margin-bottom: 18px;
  }
  .hero p b { color: var(--text); font-weight: 600; }
  .chips { display: flex; flex-wrap: wrap; gap: 8px; }
  .chip {
    background: var(--bg-soft);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--muted);
    font: 12.5px var(--mono);
    padding: 7px 11px;
    cursor: pointer;
    transition: color .15s ease, border-color .15s ease;
  }
  .chip:hover { color: var(--text); border-color: var(--border-strong); }

  /* banner */
  .banner {
    display: none;
    margin-top: 14px;
    padding: 10px 13px;
    background: rgba(224, 108, 117, .07);
    border: 1px solid rgba(224, 108, 117, .3);
    border-radius: var(--radius);
    color: #eda4ab;
    font-size: 13px;
  }
  .banner.on { display: block; }
  .banner code, .banner b { font-family: var(--mono); color: var(--accent); font-weight: 600; }

  /* ---------- footer / composer ---------- */
  footer {
    flex: none;
    border-top: 1px solid var(--border);
    background: var(--bg);
    padding: 0 20px 14px;
  }
  .composer, .hint, .status {
    max-width: 760px;
    margin-left: auto;
    margin-right: auto;
  }
  .composer {
    display: flex;
    gap: 10px;
    align-items: flex-end;
    padding-top: 4px;
  }
  textarea {
    flex: 1;
    resize: none;
    min-height: 46px;
    max-height: 160px;
    padding: 12px 13px;
    background: var(--bg-soft);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--text);
    font: 14.5px/1.5 var(--sans);
    outline: none;
    transition: border-color .15s ease;
  }
  textarea:focus { border-color: var(--border-strong); }
  textarea::placeholder { color: var(--dim); }
  .send {
    width: 46px; height: 46px;
    border: 0;
    border-radius: var(--radius);
    background: var(--accent);
    color: #0a0a0a;
    cursor: pointer;
    display: grid; place-items: center;
    transition: filter .15s ease;
  }
  .send:hover { filter: brightness(1.08); }
  .send:active { filter: brightness(.95); }
  .send:disabled { opacity: .45; cursor: not-allowed; }
  .send svg { width: 16px; height: 16px; }
  .hint {
    display: flex;
    justify-content: space-between;
    gap: 8px;
    margin-top: 8px;
    font: 11.5px var(--mono);
    color: var(--dim);
  }
  kbd {
    font: 11px var(--mono);
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 3px;
    padding: 1px 5px;
    color: var(--muted);
  }

  /* ---------- settings ---------- */
  .overlay {
    position: fixed;
    inset: 0;
    z-index: 50;
    background: rgba(0, 0, 0, .62);
    display: none;
    place-items: center;
  }
  .overlay.on { display: grid; }
  .sheet {
    width: min(440px, calc(100vw - 32px));
    max-height: calc(100vh - 60px);
    overflow-y: auto;
    background: var(--bg-soft);
    border: 1px solid var(--border-strong);
    border-radius: 8px;
    padding: 22px;
    animation: fadeIn .15s ease both;
  }
  .sheet h3 {
    font: 600 13px var(--mono);
    text-transform: uppercase;
    letter-spacing: .08em;
    color: var(--text);
    margin-bottom: 4px;
  }
  .sheet .sub {
    font-size: 12.5px;
    color: var(--muted);
    margin-bottom: 18px;
  }
  .sheet .sub span { font-family: var(--mono); font-size: 11.5px; color: var(--purple); word-break: break-all; }
  .field { margin-bottom: 14px; }
  .field label {
    display: block;
    font: 11px var(--mono);
    text-transform: uppercase;
    letter-spacing: .07em;
    color: var(--muted);
    margin-bottom: 6px;
  }
  .field input, .field select, .field textarea {
    width: 100%;
    padding: 9px 11px;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--text);
    font: 13.5px var(--sans);
    outline: none;
    transition: border-color .15s ease;
  }
  .field input:focus, .field select:focus, .field textarea:focus { border-color: var(--border-strong); }
  .field input::placeholder { color: var(--dim); }
  .field textarea { font-size: 13px; line-height: 1.5; resize: vertical; min-height: 84px; }
  .field select option { background: var(--bg-soft); }
  .field .hint {
    font: 11.5px var(--sans);
    color: var(--dim);
    margin-top: 5px;
    display: block;
  }
  .keyrow { display: flex; gap: 8px; }
  .keyrow input { flex: 1; font-family: var(--mono); font-size: 13px; }
  .keyrow .eye {
    width: 42px;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--muted);
    cursor: pointer;
    display: grid; place-items: center;
    transition: color .15s ease, border-color .15s ease;
  }
  .keyrow .eye:hover { color: var(--text); border-color: var(--border-strong); }
  .keyrow .eye svg { width: 15px; height: 15px; }
  .sheet .actions { display: flex; gap: 10px; margin-top: 20px; }
  .btn {
    flex: 1;
    padding: 9px 12px;
    background: transparent;
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--text);
    font: 500 13px var(--sans);
    cursor: pointer;
    transition: border-color .15s ease, background .15s ease;
  }
  .btn:hover { border-color: var(--border-strong); background: var(--surface); }
  .btn.primary {
    background: var(--accent);
    border-color: var(--accent);
    color: #0a0a0a;
    font-weight: 600;
  }
  .btn.primary:hover { filter: brightness(1.06); background: var(--accent); }

  .toast {
    position: fixed;
    bottom: 24px;
    left: 50%;
    transform: translateX(-50%) translateY(6px);
    background: var(--surface);
    border: 1px solid var(--border-strong);
    border-radius: var(--radius);
    color: var(--text);
    padding: 9px 16px;
    font: 13px var(--mono);
    opacity: 0;
    pointer-events: none;
    z-index: 60;
    transition: opacity .2s ease, transform .2s ease;
  }
  .toast.on { opacity: 1; transform: translateX(-50%) translateY(0); }

  @media (max-width: 640px) {
    .brand .tag { display: none; }
    .msg.user { max-width: 92%; }
  }
  @media (prefers-reduced-motion: reduce) {
    * { animation: none !important; transition: none !important; }
  }
</style>
</head>
<body>
<div class="app">
  <header>
    <div class="brand">
      <h1>screenmate<span class="cursor" aria-hidden="true"></span></h1>
      <span class="tag">coding agent · экран + код</span>
    </div>
    <div class="headright">
      <div class="mode" id="mode" title="Tab / клик — сменить режим">
        <button type="button" data-mode="build" class="active">build</button>
        <button type="button" data-mode="plan">plan</button>
      </div>
      <button class="iconbtn" id="gearBtn" title="Настройки" aria-label="Настройки">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>
      </button>
    </div>
  </header>

  <main class="chat" id="chat">
    <div class="hero" id="hero">
      <h2>Привет. Чем займёмся?</h2>
      <p>Опиши задачу — агент прочитает код, снимет скриншот экрана и поможет. Режим <b>build</b> правит файлы и запускает команды, <b>plan</b> только анализирует.</p>
      <div class="chips">
        <button class="chip" type="button">что видно на экране?</button>
        <button class="chip" type="button">объясни структуру репозитория</button>
        <button class="chip" type="button">найди, где падает тест</button>
      </div>
    </div>
    <div class="banner" id="banner">Нет API-ключа. Нажми <b>Настройки</b> (шестерёнка справа) и введи ключ провайдера.</div>
    <div id="stream"></div>
    <div class="status" id="status"><span class="dot"></span><span id="statusText"></span></div>
  </main>

  <footer>
    <div class="composer">
      <textarea id="input" rows="1" placeholder="Задача для агента…  Enter — отправить, Shift+Enter — перенос"></textarea>
      <button class="send" id="send" title="Отправить" aria-label="Отправить">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 19V5"/><path d="m5 12 7-7 7 7"/></svg>
      </button>
    </div>
    <div class="hint">
      <span><kbd>Tab</kbd> build/plan · <kbd>Ctrl</kbd>+<kbd>L</kbd> очистить</span>
      <span id="busyHint"></span>
    </div>
  </footer>
</div>

<div class="overlay" id="settingsOverlay">
  <div class="sheet" role="dialog" aria-modal="true" aria-labelledby="setTitle">
    <h3 id="setTitle">Settings</h3>
    <div class="sub">Провайдер, API-ключ и модель · <span id="cfgPath"></span></div>

    <div class="field">
      <label for="setProvider">Провайдер</label>
      <select id="setProvider"></select>
      <div class="hint" id="provStyle"></div>
    </div>

    <div class="field">
      <label for="setKey">API-ключ</label>
      <div class="keyrow">
        <input type="password" id="setKey" placeholder="sk-…" autocomplete="off" spellcheck="false"/>
        <button class="eye" id="keyEye" title="Показать/скрыть" aria-label="Показать ключ">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>
        </button>
      </div>
      <div class="hint" id="keyState">…</div>
    </div>

    <div class="field" id="customUrlField" style="display:none">
      <label for="setCustomUrl">Base URL (OpenAI-compatible)</label>
      <input type="text" id="setCustomUrl" placeholder="https://my-server/v1" autocomplete="off" spellcheck="false"/>
      <div class="hint">LM Studio / Ollama / vLLM: http://localhost:1234/v1</div>
    </div>

    <div class="field">
      <label for="setModel">Модель</label>
      <input type="text" id="setModel" placeholder="default" autocomplete="off" spellcheck="false"/>
      <div class="hint" id="modelDefault">…</div>
    </div>

    <div class="field">
      <label for="setPrompt">Системный промпт</label>
      <textarea id="setPrompt" rows="4"></textarea>
    </div>

    <div class="actions">
      <button class="btn" id="setCancel">Отмена</button>
      <button class="btn primary" id="setSave">Сохранить</button>
    </div>
  </div>
</div>
<div class="toast" id="toast"></div>
<script>
const chat = document.getElementById('chat');
const stream = document.getElementById('stream');
const input = document.getElementById('input');
const sendBtn = document.getElementById('send');
const modeEl = document.getElementById('mode');
const statusEl = document.getElementById('status');
const statusText = document.getElementById('statusText');
const banner = document.getElementById('banner');
const hero = document.getElementById('hero');
const busyHint = document.getElementById('busyHint');

let mode = 'build';
let busy = false;

function setMode(m) {
  mode = m;
  modeEl.classList.toggle('plan', m === 'plan');
  modeEl.querySelectorAll('button').forEach(b => {
    b.classList.toggle('active', b.dataset.mode === m);
  });
}
modeEl.addEventListener('click', e => {
  const b = e.target.closest('button');
  if (b) setMode(b.dataset.mode);
});

function setStatus(text) {
  if (!text) {
    statusEl.classList.remove('on');
    statusText.textContent = '';
    return;
  }
  statusEl.classList.add('on');
  statusText.textContent = text;
}

function scrollDown() {
  chat.scrollTop = chat.scrollHeight;
}

function addMsg(cls, meta, text) {
  hero && (hero.style.display = 'none');
  const div = document.createElement('div');
  div.className = 'msg ' + cls;
  if (meta) {
    const m = document.createElement('div');
    m.className = 'meta';
    m.textContent = meta;
    div.appendChild(m);
  }
  if (text != null) {
    const t = document.createElement('div');
    t.className = 'body';
    t.textContent = text;
    div.appendChild(t);
  }
  stream.appendChild(div);
  scrollDown();
  return div;
}

function addTyping() {
  hero && (hero.style.display = 'none');
  const div = document.createElement('div');
  div.className = 'msg ai';
  div.id = 'typing';
  div.innerHTML = '<div class="typing"><i></i><i></i><i></i></div>';
  stream.appendChild(div);
  scrollDown();
  return div;
}

function autosize() {
  input.style.height = 'auto';
  input.style.height = Math.min(input.scrollHeight, 160) + 'px';
}
input.addEventListener('input', autosize);

async function send() {
  const text = input.value.trim();
  if (!text || busy) return;
  input.value = '';
  autosize();
  addMsg('user', mode === 'plan' ? 'Plan' : 'Build', text);

  busy = true;
  sendBtn.disabled = true;
  busyHint.textContent = 'работаю…';
  const typing = addTyping();
  setStatus('соединение…');

  let aiDiv = null;
  let aiBody = null;
  let acc = '';

  try {
    const resp = await fetch('/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message: text, mode })
    });
    if (!resp.ok) throw new Error('HTTP ' + resp.status);
    const reader = resp.body.getReader();
    const dec = new TextDecoder();
    let buf = '';
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      let idx;
      while ((idx = buf.indexOf('\n\n')) >= 0) {
        const chunk = buf.slice(0, idx);
        buf = buf.slice(idx + 2);
        const line = chunk.split('\n').find(l => l.startsWith('data:'));
        if (!line) continue;
        let ev;
        try { ev = JSON.parse(line.slice(5).trim()); } catch { continue; }
        if (ev.type === 'status') {
          setStatus(ev.text || '');
        } else if (ev.type === 'delta') {
          if (!aiDiv) {
            typing && typing.remove();
            aiDiv = addMsg('ai', mode === 'plan' ? 'Plan' : 'Build', '');
            aiBody = aiDiv.querySelector('.body');
          }
          acc += ev.text || '';
          aiBody.textContent = acc;
          scrollDown();
        } else if (ev.type === 'image') {
          if (!aiDiv) {
            typing && typing.remove();
            aiDiv = addMsg('ai', mode === 'plan' ? 'Plan' : 'Build', '');
            aiBody = aiDiv.querySelector('.body');
          }
          const img = document.createElement('img');
          img.className = 'shot';
          img.src = 'data:image/jpeg;base64,' + ev.b64;
          aiDiv.appendChild(img);
          scrollDown();
        } else if (ev.type === 'error') {
          typing && typing.remove();
          addMsg('err', null, ev.text || 'ошибка');
        } else if (ev.type === 'done') {
          setStatus('');
          if (!aiDiv && ev.text) {
            typing && typing.remove();
            addMsg('ai', mode === 'plan' ? 'Plan' : 'Build', ev.text);
          }
        }
      }
    }
    typing && typing.remove();
  } catch (e) {
    typing && typing.remove();
    addMsg('err', null, String(e));
  } finally {
    setStatus('');
    busy = false;
    sendBtn.disabled = false;
    busyHint.textContent = '';
    scrollDown();
    input.focus();
  }
}

sendBtn.addEventListener('click', send);
input.addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    send();
  }
});
document.addEventListener('keydown', e => {
  if (e.key === 'Tab') {
    e.preventDefault();
    setMode(mode === 'build' ? 'plan' : 'build');
  }
  if (e.ctrlKey && e.key.toLowerCase() === 'l') {
    e.preventDefault();
    fetch('/clear', { method: 'POST' }).then(() => {
      stream.innerHTML = '';
      hero && (hero.style.display = '');
    });
  }
});

document.querySelectorAll('.chip').forEach(chip => {
  chip.addEventListener('click', () => {
    input.value = chip.textContent;
    autosize();
    input.focus();
  });
});

fetch('/health').then(r => r.json()).then(j => {
  if (j && j.need_auth) banner.classList.add('on');
}).catch(() => {});

/* ---------- settings ---------- */
const overlay = document.getElementById('settingsOverlay');
const setProvider = document.getElementById('setProvider');
const setKey = document.getElementById('setKey');
const keyState = document.getElementById('keyState');
const setModel = document.getElementById('setModel');
const modelDefault = document.getElementById('modelDefault');
const setPrompt = document.getElementById('setPrompt');
const setCustomUrl = document.getElementById('setCustomUrl');
const customUrlField = document.getElementById('customUrlField');
const provStyle = document.getElementById('provStyle');
const cfgPath = document.getElementById('cfgPath');
const toast = document.getElementById('toast');
let providers = [];

function showToast(text) {
  toast.textContent = text;
  toast.classList.add('on');
  clearTimeout(showToast._t);
  showToast._t = setTimeout(() => toast.classList.remove('on'), 2600);
}

function currentProv() {
  return providers.find(p => p.id === setProvider.value) || providers[0];
}

function refreshProviderFields() {
  const p = currentProv();
  if (!p) return;
  provStyle.textContent = p.style + ' · ' + (p.base || 'custom base');
  keyState.textContent = p.has_key
    ? 'Ключ сохранён: ••••' + p.key_tail + ' (оставь поле пустым, чтобы не менять)'
    : 'Ключ не задан — введи его';
  setKey.placeholder = p.has_key ? '••••' + p.key_tail : 'вставь API-ключ…';
  modelDefault.textContent = p.current_model
    ? 'Текущая: ' + p.current_model
    : 'По умолчанию: ' + p.default_model;
  setModel.placeholder = p.default_model || 'model-id';
  customUrlField.style.display = p.id === 'custom' ? '' : 'none';
}

async function openSettings() {
  try {
    const r = await fetch('/settings');
    const s = await r.json();
    providers = s.providers || [];
    cfgPath.textContent = s.config_path || '';
    setProvider.innerHTML = '';
    for (const p of providers) {
      const o = document.createElement('option');
      o.value = p.id;
      o.textContent = p.label + (p.has_key ? ' ✓' : '');
      if (p.id === s.provider) o.selected = true;
      setProvider.appendChild(o);
    }
    setPrompt.value = s.system_prompt || '';
    setCustomUrl.value = s.custom_base_url || '';
    setKey.value = '';
    refreshProviderFields();
    overlay.classList.add('on');
    setTimeout(() => setKey.focus(), 60);
  } catch (e) {
    showToast('Не удалось загрузить настройки: ' + e);
  }
}

function closeSettings() { overlay.classList.remove('on'); }

setProvider.addEventListener('change', () => { setKey.value = ''; refreshProviderFields(); });
document.getElementById('gearBtn').addEventListener('click', openSettings);
document.getElementById('setCancel').addEventListener('click', closeSettings);
overlay.addEventListener('click', e => { if (e.target === overlay) closeSettings(); });
document.getElementById('keyEye').addEventListener('click', () => {
  setKey.type = setKey.type === 'password' ? 'text' : 'password';
});

document.getElementById('setSave').addEventListener('click', async () => {
  const body = {
    provider: setProvider.value,
    api_key: setKey.value.trim(),
    model: setModel.value.trim(),
    custom_base_url: setCustomUrl.value.trim(),
    system_prompt: setPrompt.value
  };
  try {
    const r = await fetch('/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    const j = await r.json();
    if (!r.ok || j.error) throw new Error(j.error || ('HTTP ' + r.status));
    banner.classList.remove('on');
    closeSettings();
    showToast('✓ сохранено — ' + (j.provider || ''));
  } catch (e) {
    showToast('Ошибка: ' + e);
  }
});

setMode('build');
input.focus();
</script>
</body>
</html>)HTML";

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void open_browser(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

static bool send_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = (int)send(fd, data.data() + off, (int)(data.size() - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static std::string sse_event(const std::string& type, const std::string& text_key,
                             const std::string& text) {
    std::string payload = "{\"type\":\"" + type + "\"";
    if (!text_key.empty())
        payload += ",\"" + text_key + "\":\"" + json_escape(text) + "\"";
    payload += "}\n\n";
    return "data: " + payload;
}

// ---------------------------------------------------------------------------
// server
// ---------------------------------------------------------------------------

int gui_run(int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    // hide console window feel — keep process, but user opens browser GUI
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    int srv = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return 1;
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        // try a few nearby ports
        bool bound = false;
        for (int p = port + 1; p < port + 30; ++p) {
            addr.sin_port = htons((uint16_t)p);
            if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == 0) {
                port = p;
                bound = true;
                break;
            }
        }
        if (!bound) {
            closesocket(srv);
            return 1;
        }
    }
    listen(srv, 16);

    Config cfg = Config::load();
    ToolContext base_ctx;
    base_ctx.cfg = &cfg;
    base_ctx.cwd = fs::current_path();
    std::vector<json> history;
    std::mutex mu;

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    std::cout << "ScreenMate GUI: " << url << "\n";
    open_browser(url);

    for (;;) {
        int c = (int)accept(srv, nullptr, nullptr);
        if (c < 0) continue;

        std::string req;
        char buf[4096];
        size_t hdr_end = std::string::npos;
        while (hdr_end == std::string::npos) {
            int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, (size_t)n);
            hdr_end = req.find("\r\n\r\n");
            if (req.size() > 1 << 20) break;
        }
        if (hdr_end == std::string::npos) {
            closesocket(c);
            continue;
        }

        std::string head = req.substr(0, hdr_end);
        std::string method, path;
        {
            std::istringstream is(head);
            is >> method >> path;
        }

        size_t content_len = 0;
        {
            auto pos = head.find("Content-Length:");
            if (pos == std::string::npos) pos = head.find("content-length:");
            if (pos != std::string::npos) content_len = (size_t)std::atol(head.c_str() + pos + 15);
        }
        std::string body = req.substr(hdr_end + 4);
        while (body.size() < content_len) {
            int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            body.append(buf, (size_t)n);
        }

        auto respond_json = [&](int code, const std::string& json_str) {
            const char* status = code == 200 ? "200 OK" : (code == 404 ? "404 Not Found" : "400 Bad Request");
            std::string resp = "HTTP/1.1 " + std::string(status) +
                               "\r\nContent-Type: application/json; charset=utf-8\r\n"
                               "Access-Control-Allow-Origin: *\r\n"
                               "Content-Length: " + std::to_string(json_str.size()) +
                               "\r\nConnection: close\r\n\r\n" + json_str;
            send_all(c, resp);
        };

        if (method == "GET" && (path == "/" || path == "/index.html" || path.rfind("/index", 0) == 0)) {
            std::string html = kIndexHtml;
            std::string resp =
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                "Content-Length: " + std::to_string(html.size()) +
                "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n" + html;
            send_all(c, resp);
            closesocket(c);
            continue;
        }

        if (method == "GET" && path == "/health") {
            json j = {{"ok", true}, {"need_auth", cfg.api_key().empty()}};
            respond_json(200, j.dump());
            closesocket(c);
            continue;
        }

        if (method == "GET" && path == "/settings") {
            std::lock_guard<std::mutex> lock(mu);
            json provs = json::array();
            for (const auto& d : provider_defs()) {
                std::string tail;
                if (cfg.has_key(d.id)) {
                    const std::string& k = cfg.keys.at(d.id);
                    tail = k.size() > 4 ? k.substr(k.size() - 4) : k;
                }
                std::string cur_model;
                {
                    auto it = cfg.models.find(d.id);
                    if (it != cfg.models.end()) cur_model = it->second;
                }
                provs.push_back({{"id", d.id},
                                 {"label", d.label},
                                 {"style", d.style},
                                 {"base", d.base},
                                 {"has_key", cfg.has_key(d.id)},
                                 {"key_tail", tail},
                                 {"default_model", d.model},
                                 {"current_model", cur_model}});
            }
            json j = {{"provider", cfg.provider},
                      {"providers", provs},
                      {"system_prompt", cfg.system_prompt},
                      {"custom_base_url", cfg.custom_base_url},
                      {"config_path", Config::config_file().string()},
                      {"attach_screenshot", cfg.attach_screenshot}};
            respond_json(200, j.dump());
            closesocket(c);
            continue;
        }

        if (method == "POST" && path == "/settings") {
            std::string err;
            {
                std::lock_guard<std::mutex> lock(mu);
                try {
                    json j = json::parse(body.empty() ? "{}" : body);
                    std::string prov = j.value("provider", cfg.provider);
                    if (!prov.empty()) {
                        if (find_provider(prov)) cfg.provider = prov;
                        else err = "unknown provider: " + prov;
                    }
                    if (err.empty()) {
                        std::string key = j.value("api_key", "");
                        if (!key.empty()) cfg.keys[cfg.provider] = key;
                        if (j.contains("model")) {
                            std::string m = j["model"].get<std::string>();
                            if (m.empty())
                                cfg.models.erase(cfg.provider);
                            else
                                cfg.models[cfg.provider] = m;
                        }
                        if (j.contains("custom_base_url")) {
                            std::string u = j["custom_base_url"].get<std::string>();
                            while (!u.empty() && u.back() == '/') u.pop_back();
                            cfg.custom_base_url = u;
                        }
                        if (j.contains("system_prompt")) {
                            std::string p = j["system_prompt"].get<std::string>();
                            if (!p.empty()) cfg.system_prompt = p;
                        }
                        if (j.contains("attach_screenshot"))
                            cfg.attach_screenshot = j["attach_screenshot"].get<bool>();
                        cfg.save();
                    }
                } catch (const std::exception& e) {
                    err = e.what();
                }
            }
            if (!err.empty())
                respond_json(400, json({{"error", err}}).dump());
            else
                respond_json(200,
                             json({{"ok", true},
                                   {"provider", cfg.provider},
                                   {"model", cfg.model()},
                                   {"has_key", !cfg.api_key().empty()}})
                                 .dump());
            closesocket(c);
            continue;
        }

        if (method == "POST" && path == "/clear") {
            std::lock_guard<std::mutex> lock(mu);
            history.clear();
            respond_json(200, "{\"ok\":true}");
            closesocket(c);
            continue;
        }

        if (method == "POST" && path == "/chat") {
            std::string message, mode = "build";
            try {
                json j = json::parse(body.empty() ? "{}" : body);
                message = j.value("message", j.value("prompt", ""));
                mode = j.value("mode", "build");
            } catch (...) {
            }

            // SSE headers
            std::string hdr =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream; charset=utf-8\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n";
            send_all(c, hdr);

            if (message.empty()) {
                send_all(c, sse_event("error", "text", "пустое сообщение"));
                send_all(c, sse_event("done", "text", ""));
                closesocket(c);
                continue;
            }
            if (cfg.api_key().empty()) {
                send_all(c, sse_event("error", "text",
                                      "нет API-ключа — screenmate auth login"));
                send_all(c, sse_event("done", "text", ""));
                closesocket(c);
                continue;
            }

            std::lock_guard<std::mutex> lock(mu);
            ToolContext ctx = base_ctx;
            ctx.mode = (mode == "plan") ? AgentMode::Plan : AgentMode::Build;

            AgentCallbacks cb;
            cb.on_text = [&](const std::string& d) {
                send_all(c, sse_event("delta", "text", d));
            };
            cb.on_status = [&](const std::string& s) {
                send_all(c, sse_event("status", "text", s));
            };
            cb.on_image = [&](const std::string& b64) {
                send_all(c, sse_event("image", "b64", b64));
            };

            std::string reply, err;
            try {
                reply = agent_turn(cfg, ctx, history, message, cb,
                                   ctx.mode == AgentMode::Plan ? "plan" : "build");
            } catch (const std::exception& e) {
                err = e.what();
            }
            send_all(c, sse_event("status", "text", ""));
            if (!err.empty()) send_all(c, sse_event("error", "text", err));
            send_all(c, sse_event("done", "text", reply));
            closesocket(c);
            continue;
        }

        // CORS preflight
        if (method == "OPTIONS") {
            std::string resp =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Connection: close\r\n\r\n";
            send_all(c, resp);
            closesocket(c);
            continue;
        }

        respond_json(404, "{\"error\":\"not found\"}");
        closesocket(c);
    }
    // unreachable
    // return 0;
}

}  // namespace sm
