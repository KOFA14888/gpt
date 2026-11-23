# ScreenMate AI — coding agent with screen vision (C++)

Terminal agent in the spirit of OpenCode / Claude Code, written in **C++17**:
reads and edits your project, runs shell commands, searches code, talks to an
LSP — and **sees your screen** (screenshot + window list) so it can tell which
program is open, what the buttons say, and move a real mouse cursor.

Python GUI from earlier versions lives in [`legacy/`](legacy/) and is no longer
the main app.

## Features

| Area | What you get |
| --- | --- |
| TUI | fullscreen chat, **Tab** switches `BUILD` ↔ `PLAN`, Enter sends |
| Agents | **build** — full access (files, shell, cursor); **plan** — read-only analysis |
| Tools | `read_file` `write_file` `edit_file` `list_dir` `grep` `glob` `shell` `screenshot` `list_windows` `cursor` `lsp` |
| Vision | GDI screenshot (JPEG) + `list_windows` (titles + process names) |
| Cursor | `MOVE` / `CLICK` / `RIGHT_CLICK` / `DOUBLE_CLICK` / `TYPE` via SendInput, clamped to work region |
| LSP | definition / hover via spawned language server (`pylsp`, `clangd`, …) |
| CLI | `run`, `auth login`, `serve`, `models` |
| HTTP | WinHTTP on Windows (no deps), libcurl elsewhere; long read timeout for slow free models |

## Build

### Windows (recommended)

Need Visual Studio Build Tools 2022 + CMake (or just use GitHub Actions).

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
build\Release\screenmate.exe
```

### GitHub Actions

Every push runs [`.github/workflows/build.yml`](.github/workflows/build.yml):

- **windows** — MSVC build, uploads `screenmate-windows` artifact, releases zip on `main`
- **linux-smoke** — compile + `screenmate help`

Download the binary from the Actions **Artifacts** tab (or Releases).

### Linux (smoke)

```bash
sudo apt-get install -y g++ cmake libcurl4-openssl-dev
cmake -B build && cmake --build build
./build/screenmate help
```

## Usage

```text
screenmate                 # interactive TUI
screenmate run "опиши что на экране"
screenmate run -p "план: как устроен этот репозиторий"
screenmate auth login      # OpenRouter / NVIDIA keys + model
screenmate auth status
screenmate serve --port 8765
screenmate models
```

### TUI keys

- **Tab** — BUILD (full access) / PLAN (read-only)
- **Enter** — send
- **Ctrl+C** — cancel current request, again — exit
- **Ctrl+U** — clear input line · **Ctrl+L** — clear transcript
- `/mode` `/clear` `/help` `/exit`

### First run

```bash
screenmate auth login
screenmate
```

Keys are stored in `%APPDATA%\screenmate_ai\config.json` (Windows) or
`~/.config/screenmate_ai/config.json`. You can also put `OPENROUTER_API_KEY` /
`NVIDIA_NIM_API_KEY` into `.env` next to the binary.

## Config extras

```json
{
  "lsp_servers": { ".py": "pylsp", ".cpp": "clangd" },
  "region": [0, 0, 1280, 720],
  "attach_screenshot": true,
  "max_side": 1600,
  "quality": 70
}
```

`region` clamps both screenshots and AI cursor actions to a work area.

## Layout

```text
src/           C++17 sources (agent, TUI, tools, LSP, screen, cursor, HTTP)
third_party/   nlohmann/json + stb_image_write (vendored)
.github/       Actions workflow
legacy/        old Python overlay (frozen)
```

## Security

Screenshots go only to the configured provider inside the chat request; frames
are not written to disk. Pause or don't use BUILD mode when typing passwords.
`.env` and `config.json` are gitignored.
