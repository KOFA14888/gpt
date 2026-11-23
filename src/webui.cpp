// Graphical UI: embeds a polished HTML chat and serves it on 127.0.0.1.
// Streams agent output over SSE so the frontend can animate tokens live.

#include "webui.hpp"

#include "agent.hpp"
#include "common.hpp"
#include "config.hpp"
#include "http.hpp"
#include "tools.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
    --bg: #000000;
    --bg-soft: #0a0a0b;
    --surface: #101012;
    --surface2: #17171a;
    --border: #26262a;
    --border-strong: #3c3c42;
    --text: #f5f5f7;
    --muted: #a0a0a8;
    --dim: #6a6a72;
    --accent: #ffffff;
    --accent-dim: #c8c8d0;
    --purple: #cfcfd6;
    --green: #e2e2e8;
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
  /* ---------- galaxy starfield ---------- */
  body::before {
    content: '';
    position: fixed;
    inset: 0;
    pointer-events: none;
    z-index: 0;
    background:
      radial-gradient(1.5px 1.5px at 8% 14%, rgba(255,255,255,.9), transparent 50%),
      radial-gradient(1px 1px at 16% 62%, rgba(255,255,255,.55), transparent 50%),
      radial-gradient(1.5px 1.5px at 24% 32%, rgba(255,255,255,.75), transparent 50%),
      radial-gradient(1px 1px at 31% 84%, rgba(255,255,255,.45), transparent 50%),
      radial-gradient(2px 2px at 38% 8%, rgba(255,255,255,.85), transparent 50%),
      radial-gradient(1px 1px at 44% 47%, rgba(255,255,255,.5), transparent 50%),
      radial-gradient(1.5px 1.5px at 52% 71%, rgba(255,255,255,.7), transparent 50%),
      radial-gradient(1px 1px at 58% 23%, rgba(255,255,255,.5), transparent 50%),
      radial-gradient(2px 2px at 65% 91%, rgba(255,255,255,.8), transparent 50%),
      radial-gradient(1px 1px at 71% 38%, rgba(255,255,255,.45), transparent 50%),
      radial-gradient(1.5px 1.5px at 78% 12%, rgba(255,255,255,.75), transparent 50%),
      radial-gradient(1px 1px at 84% 57%, rgba(255,255,255,.55), transparent 50%),
      radial-gradient(1.5px 1.5px at 91% 78%, rgba(255,255,255,.65), transparent 50%),
      radial-gradient(1px 1px at 96% 29%, rgba(255,255,255,.4), transparent 50%),
      radial-gradient(1px 1px at 5% 88%, rgba(255,255,255,.5), transparent 50%),
      radial-gradient(1.5px 1.5px at 12% 41%, rgba(255,255,255,.6), transparent 50%),
      radial-gradient(1px 1px at 27% 6%, rgba(255,255,255,.45), transparent 50%),
      radial-gradient(1px 1px at 49% 94%, rgba(255,255,255,.5), transparent 50%),
      radial-gradient(1.5px 1.5px at 62% 52%, rgba(255,255,255,.65), transparent 50%),
      radial-gradient(1px 1px at 88% 6%, rgba(255,255,255,.5), transparent 50%),
      radial-gradient(900px 520px at 75% -12%, rgba(255,255,255,.055), transparent 70%),
      radial-gradient(760px 460px at 5% 108%, rgba(255,255,255,.045), transparent 70%),
      radial-gradient(1100px 700px at 45% 45%, rgba(255,255,255,.02), transparent 75%);
    animation: twinkle 7s ease-in-out infinite alternate;
  }
  @keyframes twinkle { from { opacity: .82; } to { opacity: 1; } }
  .app { height: 100%; display: flex; flex-direction: column; position: relative; z-index: 1; }

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
    background: rgba(0, 0, 0, .72);
    backdrop-filter: blur(8px);
    position: relative;
    z-index: 30;
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
  .mode button.active { background: var(--surface2); color: var(--text); box-shadow: inset 0 -2px 0 var(--accent); }

  .iconbtn {
    height: 30px;
    display: inline-flex;
    align-items: center;
    gap: 7px;
    padding: 0 10px;
    background: transparent;
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--muted);
    font: 12px var(--mono);
    cursor: pointer;
    transition: color .15s ease, border-color .15s ease;
  }
  .iconbtn:hover, .iconbtn.on { color: var(--text); border-color: var(--border-strong); }
  .iconbtn svg { width: 14px; height: 14px; flex: none; }
  .iconbtn.sq { width: 30px; padding: 0; justify-content: center; }

  /* ---------- sessions dropdown ---------- */
  .sesswrap { position: relative; }
  .sesspanel {
    position: absolute;
    top: calc(100% + 8px);
    right: 0;
    width: 320px;
    max-height: 70vh;
    overflow-y: auto;
    background: var(--bg-soft);
    border: 1px solid var(--border-strong);
    border-radius: 8px;
    padding: 10px;
    display: none;
    z-index: 60;
    animation: fadeIn .14s ease both;
  }
  .sesspanel.on { display: block; }
  .sesspanel .newsess {
    width: 100%;
    padding: 8px 10px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--text);
    font: 12.5px var(--mono);
    cursor: pointer;
    margin-bottom: 8px;
    transition: border-color .15s ease;
  }
  .sesspanel .newsess:hover { border-color: var(--border-strong); }
  .srow {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 7px 9px;
    border-radius: var(--radius);
    font: 12.5px var(--mono);
    color: var(--muted);
    cursor: pointer;
    transition: background .1s ease, color .1s ease;
  }
  .srow:hover { background: var(--surface); color: var(--text); }
  .srow.active { color: var(--text); background: var(--surface2); box-shadow: inset 2px 0 0 var(--accent); }
  .srow .sname { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .srow .sx {
    color: var(--dim);
    font-size: 14px;
    line-height: 1;
    padding: 0 3px;
    border-radius: 3px;
    flex: none;
  }
  .srow .sx:hover { color: var(--red); background: rgba(224,108,117,.12); }
  .sessdiv {
    font: 10.5px var(--mono);
    text-transform: uppercase;
    letter-spacing: .08em;
    color: var(--dim);
    padding: 10px 9px 4px;
    border-top: 1px solid var(--border);
    margin-top: 6px;
  }
  .srow .hbtn {
    background: transparent;
    border: 1px solid var(--border);
    border-radius: 4px;
    color: var(--dim);
    font: 11px var(--mono);
    padding: 2px 7px;
    cursor: pointer;
    flex: none;
  }
  .srow .hbtn:hover { color: var(--text); border-color: var(--border-strong); }
  .srow .hbtn.del:hover { color: var(--red); border-color: rgba(224,108,117,.4); }
  .sessempty { font: 12px var(--mono); color: var(--dim); padding: 6px 9px; }

  /* ---------- chat ---------- */
  .chat { flex: 1; overflow-y: auto; scroll-behavior: smooth; padding: 0 20px; }
  .hero, .banner, #stream, .status, .ctxbar {
    max-width: 760px;
    margin-left: auto;
    margin-right: auto;
  }
  .chat::-webkit-scrollbar { width: 10px; }
  .chat::-webkit-scrollbar-thumb { background: #232327; border: 3px solid var(--bg); border-radius: 6px; }
  .chat::-webkit-scrollbar-thumb:hover { background: #333338; }

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
    background: rgba(255,255,255,.05);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 10px 14px;
  }
  .msg.user .meta { color: var(--muted); }
  .msg.ai .meta { color: var(--text); }
  .msg.ai .body { color: var(--text); }
  .msg.sys {
    text-align: center;
    font: 12px var(--mono);
    color: var(--muted);
  }
  .msg.err {
    background: rgba(224, 108, 117, .07);
    border: 1px solid rgba(224, 108, 117, .35);
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

  .status {
    min-height: 20px;
    padding-top: 10px;
    font: 12px var(--mono);
    color: var(--text);
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

  .typing { display: inline-flex; gap: 5px; padding: 4px 0; }
  .typing i {
    width: 5px; height: 5px;
    background: var(--dim);
    animation: typePulse 1.1s ease-in-out infinite;
  }
  .typing i:nth-child(2) { animation-delay: .18s; }
  .typing i:nth-child(3) { animation-delay: .36s; }
  @keyframes typePulse { 0%, 100% { opacity: .3; } 50% { opacity: 1; background: #ffffff; } }

  .hero { padding: 56px 0 8px; }
  .hero h2 { font-size: 21px; font-weight: 650; letter-spacing: -.01em; margin-bottom: 8px; }
  .hero p { color: var(--muted); font-size: 14px; max-width: 540px; margin-bottom: 18px; }
  .hero p b { color: var(--text); font-weight: 600; }
  .chips { display: flex; flex-wrap: wrap; gap: 8px; }
  .chip {
    background: rgba(255,255,255,.04);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--muted);
    font: 12.5px var(--mono);
    padding: 7px 11px;
    cursor: pointer;
    transition: color .15s ease, border-color .15s ease;
  }
  .chip:hover { color: var(--text); border-color: var(--border-strong); }

  .banner {
    display: none;
    margin-top: 14px;
    padding: 10px 13px;
    background: rgba(255,255,255,.05);
    border: 1px solid var(--border-strong);
    border-radius: var(--radius);
    color: var(--text);
    font-size: 13px;
  }
  .banner.on { display: block; }
  .banner code, .banner b { font-family: var(--mono); color: var(--accent); font-weight: 600; }

  /* ---------- footer ---------- */
  footer {
    flex: none;
    border-top: 1px solid var(--border);
    background: rgba(0, 0, 0, .78);
    backdrop-filter: blur(8px);
    padding: 0 20px 14px;
    position: relative;
    z-index: 20;
  }
  .ctxbar { display: flex; gap: 10px; align-items: center; padding-top: 10px; position: relative; }
  .folderbtn {
    display: flex;
    align-items: center;
    gap: 7px;
    background: var(--bg-soft);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--muted);
    font: 12px var(--mono);
    padding: 6px 10px;
    cursor: pointer;
    max-width: 55%;
    overflow: hidden;
    transition: color .15s ease, border-color .15s ease;
  }
  .folderbtn:hover, .folderbtn.on { color: var(--text); border-color: var(--border-strong); }
  .folderbtn svg { width: 13px; height: 13px; flex: none; }
  .folderbtn .fname { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .modelsel {
    margin-left: auto;
    background: var(--bg-soft);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--text);
    font: 12.5px var(--mono);
    padding: 6px 8px;
    max-width: 45%;
    cursor: pointer;
    outline: none;
    transition: border-color .15s ease;
  }
  .modelsel:hover, .modelsel:focus { border-color: var(--border-strong); }
  .modelsel option, .modelsel optgroup { background: var(--bg-soft); color: var(--text); }

  /* folder popover */
  .folderpop {
    position: absolute;
    bottom: calc(100% + 8px);
    left: 0;
    width: 400px;
    max-width: calc(100vw - 40px);
    background: var(--bg-soft);
    border: 1px solid var(--border-strong);
    border-radius: 8px;
    padding: 12px;
    display: none;
    z-index: 60;
    animation: fadeIn .14s ease both;
  }
  .folderpop.on { display: block; }
  .folderpop h4 {
    font: 600 11px var(--mono);
    text-transform: uppercase;
    letter-spacing: .08em;
    color: var(--muted);
    margin-bottom: 8px;
  }
  .fbpath {
    font: 12px var(--mono);
    color: var(--text);
    word-break: break-all;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 7px 9px;
    margin-bottom: 8px;
  }
  .fblist {
    max-height: 220px;
    overflow-y: auto;
    border: 1px solid var(--border);
    border-radius: var(--radius);
    background: var(--surface);
  }
  .fbrow {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 7px 11px;
    font: 13px var(--mono);
    color: var(--muted);
    cursor: pointer;
    border-bottom: 1px solid #1a1a1d;
    transition: background .1s ease, color .1s ease;
  }
  .fbrow:last-child { border-bottom: 0; }
  .fbrow:hover { background: var(--surface2); color: var(--text); }
  .fbrow .ic { color: var(--accent-dim); flex: none; }
  .fbinput {
    width: 100%;
    margin-top: 8px;
    padding: 8px 10px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--text);
    font: 13px var(--mono);
    outline: none;
  }
  .fbinput:focus { border-color: var(--border-strong); }
  .fbactions { display: flex; gap: 8px; margin-top: 8px; }

  .composer { max-width: 760px; margin: 0 auto; display: flex; gap: 10px; align-items: flex-end; padding-top: 10px; }
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
    color: #000000;
    cursor: pointer;
    display: grid; place-items: center;
    transition: filter .15s ease;
  }
  .send:hover { filter: brightness(.88); }
  .send:active { filter: brightness(.75); }
  .send:disabled { opacity: .4; cursor: not-allowed; }
  .send svg { width: 16px; height: 16px; }
  .hint {
    max-width: 760px;
    margin: 8px auto 0;
    display: flex;
    justify-content: space-between;
    gap: 8px;
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

  /* ---------- modals ---------- */
  .overlay {
    position: fixed;
    inset: 0;
    z-index: 50;
    background: rgba(0, 0, 0, .7);
    display: none;
    place-items: center;
  }
  .overlay.on { display: grid; }
  .sheet {
    width: min(460px, calc(100vw - 32px));
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
  .sheet .sub { font-size: 12.5px; color: var(--muted); margin-bottom: 18px; }
  .sheet .sub span { font-family: var(--mono); font-size: 11.5px; color: var(--accent-dim); word-break: break-all; }
  .field { margin-bottom: 14px; }
  .field label {
    display: block;
    font: 11px var(--mono);
    text-transform: uppercase;
    letter-spacing: .07em;
    color: var(--muted);
    margin-bottom: 6px;
  }
  .field input, .field select {
    width: 100%;
    padding: 9px 11px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--text);
    font: 13.5px var(--sans);
    outline: none;
    transition: border-color .15s ease;
  }
  .field input:focus, .field select:focus { border-color: var(--border-strong); }
  .field input::placeholder { color: var(--dim); }
  .field select option { background: var(--bg-soft); }
  .field .hint {
    display: block;
    font: 11.5px var(--sans);
    color: var(--dim);
    margin-top: 5px;
    max-width: none;
    margin-left: 0;
  }
  .keyrow { display: flex; gap: 8px; }
  .keyrow input { flex: 1; font-family: var(--mono); font-size: 13px; }
  .keyrow .eye {
    width: 42px;
    background: var(--surface);
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
    color: #000000;
    font-weight: 600;
  }
  .btn.primary:hover { filter: brightness(.9); background: var(--accent); }

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
    .sesspanel { width: calc(100vw - 32px); right: -60px; }
    .folderpop { width: calc(100vw - 40px); }
    .msg.user { max-width: 92%; }
    .iconbtn .lbl { display: none; }
  }
  @media (prefers-reduced-motion: reduce) {
    * { animation: none !important; transition: none !important; }
    body::before { opacity: .92; }
  }
</style>
</head>
<body>
<div class="app">
  <header>
    <div class="brand">
      <h1>screenmate<span class="cursor" aria-hidden="true"></span></h1>
    </div>
    <div class="headright">
      <div class="mode" id="mode" title="Tab / клик — сменить режим">
        <button type="button" data-mode="build" class="active">build</button>
        <button type="button" data-mode="plan">plan</button>
      </div>
      <div class="sesswrap">
        <button class="iconbtn" id="sessBtn" title="Сессии">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M4 6h16M4 12h16M4 18h10"/></svg>
          <span class="lbl">сессии</span>
        </button>
        <div class="sesspanel" id="sessPanel">
          <button class="newsess" id="sessNew">+ новая сессия</button>
          <div id="sessActive"></div>
          <div id="sessHistWrap" style="display:none">
            <div class="sessdiv">история</div>
            <div id="sessHist"></div>
          </div>
        </div>
      </div>
      <button class="iconbtn sq" id="gearBtn" title="Настройки" aria-label="Настройки">
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
    <div class="ctxbar">
      <button class="folderbtn" id="folderBtn" title="Папка проекта">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>
        <span class="fname" id="folderName">…</span>
      </button>
      <select class="modelsel" id="modelSel" title="Модель">
        <option value="">модели…</option>
      </select>

      <div class="folderpop" id="folderPop">
        <h4>Папка проекта</h4>
        <div class="fbpath" id="fbPath">…</div>
        <div class="fblist" id="fbList"></div>
        <input class="fbinput" id="fbInput" placeholder="путь и Enter…" autocomplete="off" spellcheck="false"/>
        <div class="fbactions">
          <button class="btn" id="fbCancel">Отмена</button>
          <button class="btn primary" id="fbOk">Выбрать</button>
        </div>
      </div>
    </div>
    <div class="composer">
      <textarea id="input" rows="1" placeholder="Задача для агента…  Enter — отправить, Shift+Enter — перенос"></textarea>
      <button class="send" id="send" title="Отправить" aria-label="Отправить">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 19V5"/><path d="m5 12 7-7 7 7"/></svg>
      </button>
    </div>
    <div class="hint">
      <span><kbd>Tab</kbd> build/plan</span>
    </div>
  </footer>
</div>

<!-- settings -->
<div class="overlay" id="settingsOverlay">
  <div class="sheet" role="dialog" aria-modal="true" aria-labelledby="setTitle">
    <h3 id="setTitle">Settings</h3>
    <div class="sub">Провайдер и ключ · <span id="cfgPath"></span></div>

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
const toast = document.getElementById('toast');

let mode = 'build';
let busy = false;
let sessionsData = [];
let currentSid = '';
let cwdStr = '';

function showToast(text) {
  toast.textContent = text;
  toast.classList.add('on');
  clearTimeout(showToast._t);
  showToast._t = setTimeout(() => toast.classList.remove('on'), 2600);
}

/* ---------- mode ---------- */
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

/* ---------- sessions dropdown ---------- */
const sessBtn = document.getElementById('sessBtn');
const sessPanel = document.getElementById('sessPanel');
const sessActive = document.getElementById('sessActive');
const sessHistWrap = document.getElementById('sessHistWrap');
const sessHist = document.getElementById('sessHist');

function textOf(m) {
  const c = m.content;
  if (typeof c === 'string') return c;
  if (Array.isArray(c)) {
    return c.filter(p => p && p.type === 'text').map(p => p.text || '').join('\n');
  }
  return '';
}

function renderMessages(msgs) {
  stream.innerHTML = '';
  if (!msgs || !msgs.length) {
    hero && (hero.style.display = '');
    scrollDown();
    return;
  }
  for (const m of msgs) {
    if (m.role === 'user') {
      const t = textOf(m);
      if (t) addMsg('user', 'вы', t);
    } else if (m.role === 'assistant' && !m.tool_calls) {
      const t = textOf(m);
      if (t) addMsg('ai', 'screenmate', t);
    }
  }
  scrollDown();
}

function mkRow(name) {
  const row = document.createElement('div');
  row.className = 'srow';
  const nm = document.createElement('span');
  nm.className = 'sname';
  nm.textContent = name;
  nm.title = name;
  row.appendChild(nm);
  return { row, nm };
}

function renderSessions() {
  const open = sessionsData.filter(s => !s.closed);
  const closed = sessionsData.filter(s => s.closed);
  sessActive.innerHTML = '';
  for (const s of open) {
    const { row, nm } = mkRow(s.name);
    if (s.id === currentSid) row.classList.add('active');
    const x = document.createElement('span');
    x.className = 'sx';
    x.textContent = '×';
    x.title = 'Закрыть сессию';
    x.addEventListener('click', ev => { ev.stopPropagation(); closeSession(s.id); });
    row.appendChild(x);
    row.addEventListener('click', () => { if (s.id !== currentSid) selectSession(s.id); });
    sessActive.appendChild(row);
  }
  if (!open.length) {
    const e = document.createElement('div');
    e.className = 'sessempty';
    e.textContent = 'нет открытых сессий';
    sessActive.appendChild(e);
  }
  sessHistWrap.style.display = closed.length ? '' : 'none';
  sessHist.innerHTML = '';
  for (const s of closed) {
    const { row, nm } = mkRow(s.name);
    const resume = document.createElement('button');
    resume.className = 'hbtn';
    resume.textContent = 'открыть';
    resume.addEventListener('click', ev => { ev.stopPropagation(); resumeSession(s.id); });
    const del = document.createElement('button');
    del.className = 'hbtn del';
    del.textContent = '×';
    del.title = 'Удалить';
    del.addEventListener('click', ev => { ev.stopPropagation(); deleteSession(s.id); });
    row.appendChild(resume);
    row.appendChild(del);
    sessHist.appendChild(row);
  }
}

async function loadSessions(renderView) {
  try {
    const j = await (await fetch('/sessions')).json();
    sessionsData = j.sessions || [];
    currentSid = j.current || '';
    if (j.cwd) { cwdStr = j.cwd; updateFolderLabel(); }
    renderSessions();
    if (renderView) renderMessages(j.messages);
  } catch (e) {
    showToast('сессии: ' + e);
  }
}

async function refreshSessions() {
  try {
    const j = await (await fetch('/sessions')).json();
    const prev = currentSid;
    sessionsData = j.sessions || [];
    currentSid = j.current || '';
    if (currentSid !== prev) renderMessages(j.messages);
    renderSessions();
  } catch (e) { /* ignore */ }
}

async function selectSession(id) {
  if (busy) return;
  try {
    const r = await fetch('/sessions/select', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id })
    });
    const j = await r.json();
    if (!r.ok) throw new Error(j.error || 'HTTP ' + r.status);
    currentSid = j.current;
    renderMessages(j.messages);
    renderSessions();
    sessPanel.classList.remove('on');
    sessBtn.classList.remove('on');
  } catch (e) {
    showToast('сессия: ' + e);
  }
}

async function closeSession(id) {
  if (busy) return;
  try {
    const r = await fetch('/sessions/close', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id })
    });
    const j = await r.json();
    if (!r.ok) throw new Error(j.error || 'HTTP ' + r.status);
    await loadSessions(j.current !== currentSid);
  } catch (e) {
    showToast('закрыть: ' + e);
  }
}

async function resumeSession(id) {
  if (busy) return;
  try {
    const r = await fetch('/sessions/resume', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id })
    });
    const j = await r.json();
    if (!r.ok) throw new Error(j.error || 'HTTP ' + r.status);
    currentSid = j.current;
    renderMessages(j.messages);
    await loadSessions(false);
    sessPanel.classList.remove('on');
    sessBtn.classList.remove('on');
    showToast('сессия возобновлена');
  } catch (e) {
    showToast('возобновить: ' + e);
  }
}

async function deleteSession(id) {
  try {
    await fetch('/sessions/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id })
    });
    await loadSessions(false);
  } catch (e) {
    showToast('удалить: ' + e);
  }
}

sessBtn.addEventListener('click', e => {
  e.stopPropagation();
  const on = sessPanel.classList.toggle('on');
  sessBtn.classList.toggle('on', on);
  folderPop.classList.remove('on');
  folderBtn.classList.remove('on');
  if (on) loadSessions(false);
});
document.getElementById('sessNew').addEventListener('click', async () => {
  if (busy) return;
  try {
    await fetch('/sessions/new', { method: 'POST' });
    await loadSessions(true);
    sessPanel.classList.remove('on');
    sessBtn.classList.remove('on');
    input.focus();
  } catch (e) {
    showToast('новая: ' + e);
  }
});
document.addEventListener('click', e => {
  if (!sessPanel.contains(e.target) && !sessBtn.contains(e.target)) {
    sessPanel.classList.remove('on');
    sessBtn.classList.remove('on');
  }
  if (!folderPop.contains(e.target) && !folderBtn.contains(e.target)) {
    folderPop.classList.remove('on');
    folderBtn.classList.remove('on');
  }
});

/* ---------- models ---------- */
const modelSel = document.getElementById('modelSel');

async function loadModels() {
  modelSel.innerHTML = '<option value="">загрузка…</option>';
  try {
    const j = await (await fetch('/models')).json();
    modelSel.innerHTML = '';
    const list = j.models || [];
    const cur = j.current || '';
    const fill = (ids, label) => {
      if (!ids.length) return null;
      let parent = modelSel;
      if (label) {
        const og = document.createElement('optgroup');
        og.label = label;
        modelSel.appendChild(og);
        parent = og;
      }
      for (const id of ids) {
        const o = document.createElement('option');
        o.value = id;
        o.textContent = id;
        parent.appendChild(o);
      }
      return parent;
    };
    if (list.length) {
      const free = list.filter(id => id.indexOf(':free') !== -1);
      const rest = list.filter(id => id.indexOf(':free') === -1);
      if (free.length && rest.length) {
        fill(free, 'бесплатные');
        fill(rest, 'платные / остальные');
      } else {
        fill(list, null);
      }
      if (cur && !list.includes(cur)) {
        const o = document.createElement('option');
        o.value = cur;
        o.textContent = cur + ' (текущая)';
        modelSel.appendChild(o);
      }
      modelSel.value = cur || list[0];
    } else {
      const o = document.createElement('option');
      o.value = cur || 'default';
      o.textContent = (cur || 'default') + (j.error ? ' — ' + j.error : '');
      modelSel.appendChild(o);
      modelSel.value = o.value;
    }
  } catch (e) {
    modelSel.innerHTML = '';
    const o = document.createElement('option');
    o.value = '';
    o.textContent = 'ошибка загрузки';
    modelSel.appendChild(o);
  }
}

modelSel.addEventListener('change', async () => {
  const v = modelSel.value;
  if (!v) return;
  try {
    const r = await fetch('/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ model: v })
    });
    const j = await r.json();
    if (!r.ok) throw new Error(j.error || 'HTTP ' + r.status);
    showToast('модель: ' + v);
  } catch (e) {
    showToast('модель: ' + e);
    loadModels();
  }
});

/* ---------- folder popover ---------- */
const folderBtn = document.getElementById('folderBtn');
const folderName = document.getElementById('folderName');
const folderPop = document.getElementById('folderPop');
const fbPath = document.getElementById('fbPath');
const fbInput = document.getElementById('fbInput');
const fbList = document.getElementById('fbList');
let fbCur = '';

function baseName(p) {
  if (!p) return '';
  const parts = p.split(/[\\/]/).filter(Boolean);
  return parts.length ? parts[parts.length - 1] : p;
}
function updateFolderLabel() {
  folderName.textContent = baseName(cwdStr) || cwdStr || 'папка';
  folderBtn.title = cwdStr || 'Папка проекта';
}

async function fbBrowse(p) {
  try {
    const r = await fetch('/dirs?path=' + encodeURIComponent(p || ''));
    const j = await r.json();
    if (!r.ok) throw new Error(j.error || 'HTTP ' + r.status);
    fbCur = j.path;
    fbPath.textContent = fbCur;
    fbInput.value = fbCur;
    fbList.innerHTML = '';
    if (j.parent) {
      const { row, nm } = { row: document.createElement('div'), nm: null };
      row.className = 'fbrow';
      const ic = document.createElement('span');
      ic.className = 'ic';
      ic.textContent = '↑';
      const t = document.createElement('span');
      t.textContent = '..';
      row.appendChild(ic);
      row.appendChild(t);
      row.addEventListener('click', () => fbBrowse(j.parent));
      fbList.appendChild(row);
    }
    for (const d of (j.dirs || [])) {
      const row = document.createElement('div');
      row.className = 'fbrow';
      const ic = document.createElement('span');
      ic.className = 'ic';
      ic.textContent = '▸';
      const nm = document.createElement('span');
      nm.textContent = d.name;
      row.appendChild(ic);
      row.appendChild(nm);
      row.addEventListener('click', () => fbBrowse(d.path));
      fbList.appendChild(row);
    }
    if (!(j.dirs || []).length && !j.parent) {
      const row = document.createElement('div');
      row.className = 'fbrow';
      row.textContent = '(нет подпапок)';
      fbList.appendChild(row);
    }
  } catch (e) {
    showToast('папки: ' + e);
  }
}

folderBtn.addEventListener('click', e => {
  e.stopPropagation();
  const on = folderPop.classList.toggle('on');
  folderBtn.classList.toggle('on', on);
  sessPanel.classList.remove('on');
  sessBtn.classList.remove('on');
  if (on) fbBrowse(cwdStr || '.');
});
document.getElementById('fbCancel').addEventListener('click', () => {
  folderPop.classList.remove('on');
  folderBtn.classList.remove('on');
});
fbInput.addEventListener('keydown', e => {
  if (e.key === 'Enter') { e.preventDefault(); fbBrowse(fbInput.value.trim()); }
});
document.getElementById('fbOk').addEventListener('click', async () => {
  try {
    const r = await fetch('/cwd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: fbCur })
    });
    const j = await r.json();
    if (!r.ok) throw new Error(j.error || 'HTTP ' + r.status);
    cwdStr = j.path;
    updateFolderLabel();
    folderPop.classList.remove('on');
    folderBtn.classList.remove('on');
    showToast('папка: ' + baseName(cwdStr));
  } catch (e) {
    showToast('папка: ' + e);
  }
});

/* ---------- chat ---------- */
async function send() {
  const text = input.value.trim();
  if (!text || busy) return;
  input.value = '';
  autosize();
  addMsg('user', mode === 'plan' ? 'Plan' : 'Build', text);

  busy = true;
  sendBtn.disabled = true;
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
    refreshSessions();
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
const customUrlField = document.getElementById('customUrlField');
const setCustomUrl = document.getElementById('setCustomUrl');
const provStyle = document.getElementById('provStyle');
const cfgPath = document.getElementById('cfgPath');
let providers = [];

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
  customUrlField.style.display = p.id === 'custom' ? '' : 'none';
}

async function openSettings() {
  try {
    const s = await (await fetch('/settings')).json();
    providers = s.providers || [];
    cfgPath.textContent = s.config_path || '';
    setProvider.innerHTML = '';
    for (const p of providers) {
      const o = document.createElement('option');
      o.value = p.id;
      o.textContent = p.label;
      if (p.id === s.provider) o.selected = true;
      setProvider.appendChild(o);
    }
    setCustomUrl.value = s.custom_base_url || '';
    setKey.value = '';
    refreshProviderFields();
    overlay.classList.add('on');
    setTimeout(() => setKey.focus(), 60);
  } catch (e) {
    showToast('настройки: ' + e);
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
    custom_base_url: setCustomUrl.value.trim()
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
    loadModels();
  } catch (e) {
    showToast('Ошибка: ' + e);
  }
});

/* ---------- init ---------- */
setMode('build');
loadSessions(true);
loadModels();
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

static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// sessions (tabs) — persisted to config_dir/sessions.json
// ---------------------------------------------------------------------------

struct Session {
    std::string id;
    std::string name;
    bool closed = false;
    long long updated = 0;
    std::vector<json> messages;
};

static fs::path sessions_file() { return Config::config_dir() / "sessions.json"; }

static long long now_sec() {
    return (long long)time(nullptr);
}

static std::string sess_title_from(const std::string& msg) {
    std::string t;
    for (char ch : msg) {
        if (ch == '\n' || ch == '\r') ch = ' ';
        t.push_back(ch);
        if ((int)t.size() >= 42) break;
    }
    if (t.size() >= 42) t += "…";
    return t.empty() ? "новая сессия" : t;
}

static void load_sessions(std::vector<Session>& out, std::string& current) {
    out.clear();
    current.clear();
    try {
        std::ifstream in(sessions_file());
        if (in) {
            json j;
            in >> j;
            current = j.value("current", "");
            for (auto& s : j.value("sessions", json::array())) {
                Session ss;
                ss.id = s.value("id", "");
                ss.name = s.value("name", "сессия");
                ss.closed = s.value("closed", false);
                ss.updated = s.value("updated", 0);
                if (s.contains("messages") && s["messages"].is_array())
                    ss.messages = s["messages"].get<std::vector<json>>();
                if (!ss.id.empty()) out.push_back(std::move(ss));
            }
        }
    } catch (...) {
        out.clear();
    }
}

static void save_sessions(const std::vector<Session>& sessions, const std::string& current) {
    try {
        std::error_code ec;
        fs::create_directories(Config::config_dir(), ec);
        json arr = json::array();
        for (auto& s : sessions) {
            arr.push_back({{"id", s.id},
                           {"name", s.name},
                           {"closed", s.closed},
                           {"updated", s.updated},
                           {"messages", s.messages}});
        }
        json j = {{"current", current}, {"sessions", arr}};
        std::ofstream out(sessions_file(), std::ios::trunc);
        out << j.dump();
    } catch (...) {
    }
}

// fetch model catalogue for a provider (JSON array of ids)
static json fetch_model_list(const Config& c) {
    json out = json::array();
    std::map<std::string, std::string> hdrs;
    std::string st = c.style();
    std::string body;
    if (st == "anthropic") {
        if (!c.api_key().empty()) {
            hdrs["x-api-key"] = c.api_key();
            hdrs["anthropic-version"] = "2023-06-01";
        }
        body = http_get("https://api.anthropic.com/v1/models", hdrs, 20);
    } else if (st == "gemini") {
        if (!c.api_key().empty()) hdrs["x-goog-api-key"] = c.api_key();
        body = http_get(c.base_url() + "/models", hdrs, 20);
    } else {
        if (!c.api_key().empty()) hdrs["Authorization"] = "Bearer " + c.api_key();
        body = http_get(c.base_url() + "/models", hdrs, 20);
    }
    json j = json::parse(body);
    json arr = j.value("data", json::array());
    if (arr.empty()) arr = j.value("models", json::array());
    for (auto& m : arr) {
        std::string id = m.value("id", m.value("name", ""));
        if (id.empty()) continue;
        if (id.rfind("models/", 0) == 0) id = id.substr(7);
        out.push_back(id);
        if ((int)out.size() >= 500) break;
    }
    // бесплатные (OpenRouter :free) — наверх списка
    if (c.provider == "openrouter") {
        std::stable_sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
            bool fa = a.find(":free") != std::string::npos;
            bool fb = b.find(":free") != std::string::npos;
            return fa > fb;
        });
    }
    return out;
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
    std::mutex mu;

    // sessions: [0..] tabs; closed ones live in history
    std::vector<Session> sessions;
    std::string current_sid;
    load_sessions(sessions, current_sid);
    auto find_sess = [&](const std::string& id) -> Session* {
        for (auto& s : sessions)
            if (s.id == id) return &s;
        return nullptr;
    };
    auto ensure_current = [&]() {
        Session* cur = current_sid.empty() ? nullptr : find_sess(current_sid);
        if (cur && !cur->closed) return cur;
        for (auto& s : sessions)
            if (!s.closed) {
                current_sid = s.id;
                return &s;
            }
        // create first session
        Session s;
        s.id = "s" + std::to_string(now_sec()) + "x" + std::to_string(sessions.size());
        s.name = "новая сессия";
        s.updated = now_sec();
        sessions.push_back(std::move(s));
        current_sid = sessions.back().id;
        return &sessions.back();
    };
    ensure_current();
    save_sessions(sessions, current_sid);

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
                      {"custom_base_url", cfg.custom_base_url},
                      {"config_path", Config::config_file().string()},
                      {"attach_screenshot", cfg.attach_screenshot},
                      {"cwd", base_ctx.cwd.string()}};
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
            if (Session* cur = ensure_current()) cur->messages.clear();
            save_sessions(sessions, current_sid);
            respond_json(200, "{\"ok\":true}");
            closesocket(c);
            continue;
        }

        // ---------- sessions ----------
        if (method == "GET" && path == "/sessions") {
            std::lock_guard<std::mutex> lock(mu);
            ensure_current();
            json arr = json::array();
            for (auto& s : sessions)
                arr.push_back({{"id", s.id},
                               {"name", s.name},
                               {"closed", s.closed},
                               {"updated", s.updated},
                               {"current", s.id == current_sid}});
            Session* cur = find_sess(current_sid);
            json j = {{"sessions", arr},
                      {"current", current_sid},
                      {"messages", cur ? json(cur->messages) : json::array()},
                      {"cwd", base_ctx.cwd.string()}};
            respond_json(200, j.dump());
            closesocket(c);
            continue;
        }
        if (method == "POST" && path == "/sessions/new") {
            std::lock_guard<std::mutex> lock(mu);
            Session s;
            s.id = "s" + std::to_string(now_sec()) + "x" + std::to_string(sessions.size());
            s.name = "новая сессия";
            s.updated = now_sec();
            sessions.push_back(std::move(s));
            current_sid = sessions.back().id;
            save_sessions(sessions, current_sid);
            respond_json(200, json({{"ok", true}, {"current", current_sid}}).dump());
            closesocket(c);
            continue;
        }
        if (method == "POST" && path == "/sessions/select") {
            std::string err, id;
            json messages = json::array();
            {
                std::lock_guard<std::mutex> lock(mu);
                try {
                    id = json::parse(body.empty() ? "{}" : body).value("id", "");
                } catch (...) {
                }
                Session* s = id.empty() ? nullptr : find_sess(id);
                if (!s || s->closed) err = "session not found";
                else {
                    current_sid = s->id;
                    messages = s->messages;
                    save_sessions(sessions, current_sid);
                }
            }
            if (!err.empty())
                respond_json(400, json({{"error", err}}).dump());
            else
                respond_json(200, json({{"ok", true}, {"current", id}, {"messages", messages}}).dump());
            closesocket(c);
            continue;
        }
        if (method == "POST" && path == "/sessions/close") {
            std::string err, new_cur;
            {
                std::lock_guard<std::mutex> lock(mu);
                try {
                    std::string id = json::parse(body.empty() ? "{}" : body).value("id", "");
                    Session* s = id.empty() ? nullptr : find_sess(id);
                    if (!s) err = "session not found";
                    else {
                        s->closed = true;
                        s->updated = now_sec();
                        ensure_current();  // switches away if needed
                        new_cur = current_sid;
                        save_sessions(sessions, current_sid);
                    }
                } catch (const std::exception& e) {
                    err = e.what();
                }
            }
            if (!err.empty())
                respond_json(400, json({{"error", err}}).dump());
            else
                respond_json(200, json({{"ok", true}, {"current", new_cur}}).dump());
            closesocket(c);
            continue;
        }
        if (method == "POST" && path == "/sessions/resume") {
            std::string err, id;
            json messages = json::array();
            {
                std::lock_guard<std::mutex> lock(mu);
                try {
                    id = json::parse(body.empty() ? "{}" : body).value("id", "");
                } catch (...) {
                }
                Session* s = id.empty() ? nullptr : find_sess(id);
                if (!s) err = "session not found";
                else {
                    s->closed = false;
                    s->updated = now_sec();
                    current_sid = s->id;
                    messages = s->messages;
                    save_sessions(sessions, current_sid);
                }
            }
            if (!err.empty())
                respond_json(400, json({{"error", err}}).dump());
            else
                respond_json(200, json({{"ok", true}, {"current", id}, {"messages", messages}}).dump());
            closesocket(c);
            continue;
        }
        if (method == "POST" && path == "/sessions/delete") {
            std::string err;
            {
                std::lock_guard<std::mutex> lock(mu);
                try {
                    std::string id = json::parse(body.empty() ? "{}" : body).value("id", "");
                    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                                  [&](const Session& s) { return s.id == id; }),
                                   sessions.end());
                    ensure_current();
                    save_sessions(sessions, current_sid);
                } catch (const std::exception& e) {
                    err = e.what();
                }
            }
            if (!err.empty())
                respond_json(400, json({{"error", err}}).dump());
            else
                respond_json(200, "{\"ok\":true}");
            closesocket(c);
            continue;
        }

        // ---------- project folder ----------
        if (method == "GET" && path.rfind("/dirs", 0) == 0) {
            std::string start = ".";
            auto qp = path.find("path=");
            if (qp != std::string::npos) start = url_decode(path.substr(qp + 5));
            json dirs = json::array();
            std::string err;
            try {
                fs::path p(start.empty() ? "." : start);
                std::error_code ec;
                if (!fs::is_directory(p, ec)) throw std::runtime_error("не папка: " + start);
                p = fs::weakly_canonical(p, ec);
                if (ec) p = fs::absolute(p);
                for (auto& e : fs::directory_iterator(p)) {
                    if (!e.is_directory()) continue;
                    std::string name = e.path().filename().string();
                    if (name.empty()) continue;
                    dirs.push_back({{"name", name}, {"path", e.path().string()}});
                }
                std::sort(dirs.begin(), dirs.end(), [](const json& a, const json& b) {
                    return to_lower(a.value("name", "")) < to_lower(b.value("name", ""));
                });
                fs::path parent = p.parent_path();
                respond_json(200,
                             json({{"ok", true},
                                   {"path", p.string()},
                                   {"parent", parent == p ? "" : parent.string()},
                                   {"dirs", dirs}})
                                 .dump());
            } catch (const std::exception& e) {
                respond_json(400, json({{"error", e.what()}}).dump());
            }
            closesocket(c);
            continue;
        }
        if (method == "POST" && path == "/cwd") {
            std::string err, pth;
            {
                std::lock_guard<std::mutex> lock(mu);
                try {
                    pth = json::parse(body.empty() ? "{}" : body).value("path", "");
                    std::error_code ec;
                    if (pth.empty() || !fs::is_directory(pth, ec))
                        throw std::runtime_error("не папка: " + pth);
                    base_ctx.cwd = fs::weakly_canonical(pth, ec);
                    if (ec) base_ctx.cwd = fs::absolute(pth);
                } catch (const std::exception& e) {
                    err = e.what();
                }
            }
            if (!err.empty())
                respond_json(400, json({{"error", err}}).dump());
            else
                respond_json(200, json({{"ok", true}, {"path", base_ctx.cwd.string()}}).dump());
            closesocket(c);
            continue;
        }

        // ---------- model catalogue ----------
        if (method == "GET" && path == "/models") {
            Config snap;
            {
                std::lock_guard<std::mutex> lock(mu);
                snap = cfg;
            }
            json resp = {{"provider", snap.provider}, {"current", snap.model()}};
            if (snap.api_key().empty()) {
                resp["error"] = "нет API-ключа";
            } else {
                try {
                    resp["models"] = fetch_model_list(snap);
                } catch (const std::exception& e) {
                    resp["error"] = e.what();
                }
            }
            respond_json(200, resp.dump());
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
            Session* cur = ensure_current();
            if (cur->messages.empty() || cur->name == "новая сессия") {
                if (cur->messages.empty()) cur->name = sess_title_from(message);
            }
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
                reply = agent_turn(cfg, ctx, cur->messages, message, cb,
                                   ctx.mode == AgentMode::Plan ? "plan" : "build");
            } catch (const std::exception& e) {
                err = e.what();
            }
            cur->updated = now_sec();
            save_sessions(sessions, current_sid);
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
