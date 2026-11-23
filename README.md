# ScreenMate AI — console coding agent (C++)

A Claude Code-style coding agent for your repository, written in **C++17**:
reads and edits files, runs shell commands, searches code, talks to an LSP —
right in your terminal. Black & white UI, slash commands, **Tab** = build/plan.

Python GUI from earlier versions lives in [`legacy/`](legacy/) and is no longer
the main app.

## Install

### Windows (PowerShell)

```powershell
iwr https://raw.githubusercontent.com/KOFA14888/gpt/arena/d7d4a28f-gpt/install.ps1 -UseBasicParsing | iex
```

Downloads the prebuilt `screenmate.exe` from Releases (falls back to
git clone + build), installs to `%LOCALAPPDATA%\ScreenMate\bin`, adds to PATH.

### Linux / macOS

```bash
curl -fsSL https://raw.githubusercontent.com/KOFA14888/gpt/arena/d7d4a28f-gpt/install.sh | bash
```

Downloads the prebuilt Linux x64 binary from Releases (falls back to
git clone + cmake build), installs to `~/.local/bin`.

### From source

```bash
git clone https://github.com/KOFA14888/gpt
cd gpt
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/screenmate help
```

Windows: need Visual Studio Build Tools 2022 + CMake.

## Features

| Area | What you get |
| --- | --- |
| TUI | default launch: Claude Code-style terminal chat, streaming answers, **Tab** = build/plan |
| Commands | `/help` `/clear` `/mode` `/build` `/plan` `/model [id]` `/cwd [path]` `/tools` `/key` `/exit` |
| Agents | **build** — full access (files, shell); **plan** — read-only analysis |
| Tools | `read_file` `write_file` `edit_file` `list_dir` `grep` `glob` `shell` `lsp` |
| LSP | definition / hover via spawned language server (`pylsp`, `clangd`, …) |
| Providers | NVIDIA NIM, OpenRouter, OpenAI, Anthropic Claude, Google Gemini, Groq, DeepSeek, Mistral, Together, custom OpenAI-compatible — `screenmate auth login` or web **Settings ⚙** |
| CLI | `run`, `auth login`, `serve`, `web`, `models` |
| HTTP | WinHTTP on Windows (no deps), libcurl elsewhere; long read timeout for slow free models |

The agent works only with the repository (files, shell, search, LSP) — it has
**no access to your screen**: no screenshots, no window list, no cursor.

## Usage

```text
screenmate                 # terminal UI (default)
screenmate run "найди, где падает тест"
screenmate run -p "план: как устроен этот репозиторий"
screenmate auth login      # interactive: provider + API key + model
screenmate auth status
screenmate web --port 8765 # web GUI (optional)
screenmate serve --port 8765
screenmate models
```

### TUI keys & commands

- **Tab** — BUILD (full access) / PLAN (read-only)
- **Enter** — send · **Ctrl+C** — cancel, again — exit
- **Ctrl+U** — clear input line · **Ctrl+L** — clear transcript
- `/help` — list commands · `/clear` — reset chat · `/mode` — toggle mode
- `/model <id>` — switch model · `/cwd <path>` — switch project folder
- `/tools` — show active tool list · `/key` — key status · `/exit` — quit

### First run

```bash
screenmate auth login   # pick provider, paste API key, optional model
screenmate              # chat
```

Supported providers: NVIDIA NIM · OpenRouter · OpenAI · Anthropic · Google
Gemini · Groq · DeepSeek · Mistral · Together · custom (any OpenAI-compatible
endpoint, e.g. LM Studio / Ollama / vLLM).

Keys are stored in `%APPDATA%\screenmate_ai\config.json` (Windows) or
`~/.config/screenmate_ai/config.json`. You can also put
`OPENAI_API_KEY` / `ANTHROPIC_API_KEY` / `GEMINI_API_KEY` / `GROQ_API_KEY` /
`OPENROUTER_API_KEY` / `NVIDIA_NIM_API_KEY` / `DEEPSEEK_API_KEY` /
`MISTRAL_API_KEY` / `TOGETHER_API_KEY` into `.env` next to the binary.

## Build & CI

Every push runs [`.github/workflows/build.yml`](.github/workflows/build.yml):

- **windows** — MSVC build → `screenmate-windows.zip`
- **linux** — g++ build + `screenmate help` smoke → `screenmate-linux-x64.tar.gz`
- **release** — publishes both to the moving **`latest`** release:
  https://github.com/KOFA14888/gpt/releases/tag/latest

Local Linux build:

```bash
sudo apt-get install -y g++ cmake libcurl4-openssl-dev
cmake -B build && cmake --build build
./build/screenmate help
```

## Config extras

```json
{
  "lsp_servers": { ".py": "pylsp", ".cpp": "clangd" },
  "history_limit": 16
}
```

## Layout

```text
src/           C++17 sources (agent, TUI, tools, LSP, web UI, HTTP)
third_party/   nlohmann/json + Dear ImGui (vendored)
.github/       Actions workflow
install.sh     Linux/macOS installer
install.ps1    Windows installer
legacy/        old Python overlay (frozen)
```

## Security

The agent never captures your screen. In BUILD mode it can edit files and run
commands in the project folder — pause or switch to PLAN when typing
passwords. `.env` and `config.json` are gitignored.
