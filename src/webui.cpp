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
<title>ScreenMate AI</title>
<style>
  :root {
    --bg0: #0b0d12;
    --bg1: #12151d;
    --panel: rgba(22, 26, 36, 0.72);
    --stroke: rgba(255,255,255,0.08);
    --text: #e8ecf4;
    --muted: #8b93a7;
    --accent: #7c6cff;
    --accent2: #4ecdc4;
    --user: #2a2f45;
    --ai: #171b26;
    --danger: #ff6b8a;
    --ok: #3ddc97;
    --radius: 16px;
    --shadow: 0 18px 50px rgba(0,0,0,0.45);
    font-family: "Segoe UI", system-ui, -apple-system, sans-serif;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body { height: 100%; }
  body {
    color: var(--text);
    background:
      radial-gradient(1200px 600px at 10% -10%, rgba(124,108,255,0.22), transparent 55%),
      radial-gradient(900px 500px at 100% 0%, rgba(78,205,196,0.14), transparent 50%),
      radial-gradient(700px 700px at 50% 120%, rgba(255,107,138,0.10), transparent 55%),
      linear-gradient(160deg, var(--bg0), var(--bg1) 55%, #0d1018);
    overflow: hidden;
    animation: bgPulse 18s ease-in-out infinite alternate;
  }
  @keyframes bgPulse {
    from { filter: hue-rotate(0deg) saturate(1); }
    to   { filter: hue-rotate(12deg) saturate(1.08); }
  }
  .app {
    height: 100%;
    display: grid;
    grid-template-rows: auto 1fr auto;
    max-width: 920px;
    margin: 0 auto;
    padding: 18px 18px 12px;
    gap: 14px;
  }
  /* ---------- header ---------- */
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 14px 18px;
    background: var(--panel);
    border: 1px solid var(--stroke);
    border-radius: var(--radius);
    backdrop-filter: blur(16px);
    box-shadow: var(--shadow);
    animation: slideDown .55s cubic-bezier(.2,.8,.2,1) both;
  }
  @keyframes slideDown {
    from { opacity: 0; transform: translateY(-18px); }
    to   { opacity: 1; transform: none; }
  }
  .brand { display: flex; align-items: center; gap: 12px; }
  .logo {
    width: 38px; height: 38px; border-radius: 12px;
    background: conic-gradient(from 210deg, var(--accent), var(--accent2), var(--danger), var(--accent));
    box-shadow: 0 0 24px rgba(124,108,255,0.45);
    animation: spinSoft 10s linear infinite;
    position: relative;
  }
  .logo::after {
    content: ""; position: absolute; inset: 3px; border-radius: 9px;
    background: #0e1118;
  }
  @keyframes spinSoft { to { transform: rotate(360deg); } }
  .brand h1 { font-size: 16px; font-weight: 650; letter-spacing: .2px; }
  .brand p { font-size: 12px; color: var(--muted); margin-top: 2px; }
  .mode {
    position: relative;
    display: grid;
    grid-template-columns: 1fr 1fr;
    background: rgba(0,0,0,0.28);
    border: 1px solid var(--stroke);
    border-radius: 999px;
    padding: 4px;
    min-width: 190px;
    user-select: none;
    cursor: pointer;
  }
  .mode .pill {
    position: absolute;
    top: 4px; bottom: 4px; left: 4px;
    width: calc(50% - 4px);
    border-radius: 999px;
    background: linear-gradient(135deg, var(--accent), #5b8cff);
    box-shadow: 0 6px 18px rgba(124,108,255,0.4);
    transition: transform .35s cubic-bezier(.2,.9,.2,1);
    z-index: 0;
  }
  .mode.plan .pill { transform: translateX(100%); background: linear-gradient(135deg, #3ddc97, #4ecdc4); box-shadow: 0 6px 18px rgba(61,220,151,0.35); }
  .mode button {
    position: relative; z-index: 1;
    background: transparent; border: 0; color: var(--muted);
    font: 600 13px/1 inherit; padding: 10px 12px; border-radius: 999px;
    transition: color .25s ease;
    cursor: pointer;
  }
  .mode button.active { color: #fff; }
  /* ---------- chat ---------- */
  .chat {
    overflow-y: auto;
    padding: 6px 4px 18px;
    scroll-behavior: smooth;
    mask-image: linear-gradient(to bottom, transparent, #000 12px, #000 calc(100% - 8px), transparent);
  }
  .chat::-webkit-scrollbar { width: 8px; }
  .chat::-webkit-scrollbar-thumb { background: rgba(255,255,255,0.12); border-radius: 8px; }
  .msg {
    max-width: 82%;
    margin: 10px 0;
    padding: 12px 14px;
    border-radius: 16px;
    line-height: 1.5;
    font-size: 14.5px;
    white-space: pre-wrap;
    word-break: break-word;
    border: 1px solid var(--stroke);
    animation: popIn .45s cubic-bezier(.2,.9,.2,1) both;
    position: relative;
  }
  @keyframes popIn {
    from { opacity: 0; transform: translateY(14px) scale(.97); }
    to   { opacity: 1; transform: none; }
  }
  .msg.user {
    margin-left: auto;
    background: linear-gradient(160deg, #343b58, var(--user));
    border-color: rgba(124,108,255,0.28);
    box-shadow: 0 8px 24px rgba(0,0,0,0.25);
  }
  .msg.ai {
    margin-right: auto;
    background: linear-gradient(160deg, #1c2130, var(--ai));
  }
  .msg.sys {
    max-width: 100%;
    margin: 8px auto;
    text-align: center;
    background: transparent;
    border: 0;
    color: var(--muted);
    font-size: 12.5px;
    animation: fadeIn .4s ease both;
  }
  .msg.err {
    max-width: 100%;
    margin: 8px auto;
    text-align: center;
    color: var(--danger);
    background: rgba(255,107,138,0.08);
    border-color: rgba(255,107,138,0.25);
    font-size: 13px;
  }
  .msg .meta {
    font-size: 11px;
    color: var(--muted);
    margin-bottom: 6px;
    letter-spacing: .4px;
    text-transform: uppercase;
  }
  .msg.ai .meta { color: #9b93ff; }
  .msg img.shot {
    display: block;
    max-width: 100%;
    border-radius: 10px;
    margin-top: 8px;
    border: 1px solid var(--stroke);
    animation: fadeIn .5s ease .1s both;
  }
  @keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }
  /* status ticker */
  .status {
    min-height: 22px;
    font-size: 12.5px;
    color: var(--accent2);
    padding: 0 8px;
    opacity: 0;
    transform: translateY(6px);
    transition: opacity .3s ease, transform .3s ease;
  }
  .status.on { opacity: 1; transform: none; }
  .status .dot {
    display: inline-block; width: 7px; height: 7px; border-radius: 50%;
    background: var(--accent2); margin-right: 8px;
    box-shadow: 0 0 10px var(--accent2);
    animation: blink 1s ease-in-out infinite;
  }
  @keyframes blink { 50% { opacity: .25; transform: scale(.75); } }
  /* typing bubbles */
  .typing {
    display: inline-flex; gap: 5px; align-items: center;
    padding: 14px 16px;
  }
  .typing i {
    width: 7px; height: 7px; border-radius: 50%;
    background: var(--muted);
    animation: bounce 1.1s ease-in-out infinite;
  }
  .typing i:nth-child(2) { animation-delay: .15s; }
  .typing i:nth-child(3) { animation-delay: .3s; }
  @keyframes bounce {
    0%, 80%, 100% { transform: translateY(0); opacity: .4; }
    40% { transform: translateY(-7px); opacity: 1; background: var(--accent); }
  }
  /* ---------- composer ---------- */
  footer {
    background: var(--panel);
    border: 1px solid var(--stroke);
    border-radius: var(--radius);
    backdrop-filter: blur(16px);
    box-shadow: var(--shadow);
    padding: 12px;
    animation: slideUp .55s cubic-bezier(.2,.8,.2,1) .08s both;
  }
  @keyframes slideUp {
    from { opacity: 0; transform: translateY(20px); }
    to   { opacity: 1; transform: none; }
  }
  .composer {
    display: flex;
    gap: 10px;
    align-items: flex-end;
  }
  textarea {
    flex: 1;
    resize: none;
    min-height: 48px;
    max-height: 160px;
    padding: 13px 14px;
    border-radius: 14px;
    border: 1px solid var(--stroke);
    background: rgba(0,0,0,0.28);
    color: var(--text);
    font: 14.5px/1.45 inherit;
    outline: none;
    transition: border-color .25s ease, box-shadow .25s ease, transform .2s ease;
  }
  textarea:focus {
    border-color: rgba(124,108,255,0.55);
    box-shadow: 0 0 0 4px rgba(124,108,255,0.16), 0 0 24px rgba(124,108,255,0.18);
    transform: translateY(-1px);
  }
  textarea::placeholder { color: #6d7488; }
  .send {
    width: 48px; height: 48px;
    border: 0; border-radius: 14px;
    background: linear-gradient(145deg, var(--accent), #5b8cff);
    color: #fff; font-size: 18px;
    cursor: pointer;
    box-shadow: 0 10px 24px rgba(124,108,255,0.35);
    transition: transform .18s ease, box-shadow .18s ease, filter .18s ease;
    display: grid; place-items: center;
  }
  .send:hover { transform: translateY(-2px) scale(1.04); box-shadow: 0 14px 30px rgba(124,108,255,0.45); }
  .send:active { transform: scale(.96); }
  .send:disabled { filter: grayscale(.5) opacity(.55); cursor: not-allowed; transform: none; }
  .hint {
    margin-top: 8px;
    font-size: 11.5px;
    color: var(--muted);
    display: flex;
    justify-content: space-between;
    gap: 8px;
  }
  kbd {
    background: rgba(255,255,255,0.06);
    border: 1px solid var(--stroke);
    border-radius: 6px;
    padding: 1px 6px;
    font-size: 11px;
  }
  .banner {
    display: none;
    margin-bottom: 10px;
    padding: 10px 12px;
    border-radius: 12px;
    background: rgba(255,107,138,0.1);
    border: 1px solid rgba(255,107,138,0.28);
    color: #ffb3c2;
    font-size: 13px;
    animation: popIn .4s ease both;
  }
  .banner.on { display: block; }
  /* hero empty state */
  .hero {
    text-align: center;
    padding: 48px 16px 24px;
    animation: popIn .6s cubic-bezier(.2,.9,.2,1) both;
  }
  .hero .orb {
    width: 72px; height: 72px; margin: 0 auto 18px;
    border-radius: 50%;
    background: conic-gradient(from 180deg, var(--accent), var(--accent2), var(--danger), var(--accent));
    filter: blur(0.2px);
    box-shadow: 0 0 50px rgba(124,108,255,0.45);
    animation: float 4s ease-in-out infinite;
  }
  @keyframes float {
    0%,100% { transform: translateY(0); }
    50% { transform: translateY(-10px); }
  }
  .hero h2 { font-size: 22px; font-weight: 650; margin-bottom: 8px; }
  .hero p { color: var(--muted); font-size: 14px; max-width: 420px; margin: 0 auto; }
</style>
</head>
<body>
<div class="app">
  <header>
    <div class="brand">
      <div class="logo" aria-hidden="true"></div>
      <div>
        <h1>ScreenMate AI</h1>
        <p>кодинг-агент с доступом к экрану</p>
      </div>
    </div>
    <div class="mode" id="mode" title="Tab / клик — сменить режим">
      <div class="pill"></div>
      <button type="button" data-mode="build" class="active">Build</button>
      <button type="button" data-mode="plan">Plan</button>
    </div>
  </header>

  <main class="chat" id="chat">
    <div class="hero" id="hero">
      <div class="orb"></div>
      <h2>Чем займёмся?</h2>
      <p>Опиши задачу — агент прочитает код, найдёт нужное, снимет скриншот экрана и поможет. Режим <b>Build</b> правит файлы, <b>Plan</b> только анализирует.</p>
    </div>
    <div class="banner" id="banner">Нет API-ключа. Выполни <code>screenmate auth login</code> в терминале и перезапусти.</div>
    <div id="stream"></div>
  </main>

  <footer>
    <div class="status" id="status"><span class="dot"></span><span id="statusText"></span></div>
    <div class="composer">
      <textarea id="input" rows="1" placeholder="Напиши задачу…  Enter — отправить, Shift+Enter — перенос"></textarea>
      <button class="send" id="send" title="Отправить">➤</button>
    </div>
    <div class="hint">
      <span><kbd>Tab</kbd> build/plan · <kbd>Ctrl</kbd>+<kbd>L</kbd> очистить</span>
      <span id="busyHint"></span>
    </div>
  </footer>
</div>
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
    if (!aiDiv && !acc) {
      // stream ended without events — nothing to show
    }
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

fetch('/health').then(r => r.json()).then(j => {
  if (j && j.need_auth) banner.classList.add('on');
}).catch(() => {});

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
            if (req.size() > 1 << 20) break;
        }
        hdr_end = req.find("\r\n\r\n");
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
