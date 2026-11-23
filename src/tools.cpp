#include "tools.hpp"

#include "common.hpp"
#include "cursor.hpp"
#include "lsp.hpp"
#include "screen.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <regex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sm {

using json = nlohmann::json;

// ---------- helpers ----------

static fs::path resolve_in_cwd(ToolContext& ctx, const std::string& p) {
    fs::path path(p);
    if (path.is_absolute()) return path;
    return ctx.cwd / path;
}

static std::string path_display(const fs::path& p) {
    return p.generic_string();
}

static json ok(const std::string& content) {
    return {{"ok", true}, {"content", content}};
}
static json fail(const std::string& content) {
    return {{"ok", false}, {"content", content}};
}

// ---------- file tools ----------

static json tool_read_file(ToolContext& ctx, const json& args) {
    auto path = resolve_in_cwd(ctx, args.value("path", ""));
    auto text = read_file_text(path);
    if (!text) return fail("Cannot read file: " + path_display(path));
    size_t offset = args.value("offset", 0);
    size_t limit = args.value("limit", (size_t)2000);
    std::string s = *text;
    if (offset > s.size()) offset = s.size();
    std::string slice = s.substr(offset, limit);
    // number lines for model convenience
    auto lines = split_lines(slice);
    std::string out;
    size_t start_line = 1;
    for (size_t i = 0; i < offset; ++i)
        if (i < s.size() && s[i] == '\n') ++start_line;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += std::to_string(start_line + i) + "\t" + lines[i] + "\n";
    }
    if (offset + slice.size() < s.size()) out += "…[more available]";
    return ok(clamp_text(out));
}

static json tool_write_file(ToolContext& ctx, const json& args) {
    if (ctx.mode == AgentMode::Plan) return fail("plan mode: writes are disabled");
    auto path = resolve_in_cwd(ctx, args.value("path", ""));
    std::string content = args.value("content", "");
    if (!write_file_text(path, content))
        return fail("Cannot write file: " + path_display(path));
    return ok("Wrote " + std::to_string(content.size()) + " bytes to " + path_display(path));
}

static json tool_edit_file(ToolContext& ctx, const json& args) {
    if (ctx.mode == AgentMode::Plan) return fail("plan mode: edits are disabled");
    auto path = resolve_in_cwd(ctx, args.value("path", ""));
    auto text = read_file_text(path);
    if (!text) return fail("Cannot read file: " + path_display(path));
    std::string old_t = args.value("old_string", args.value("old_text", ""));
    std::string new_t = args.value("new_string", args.value("new_text", ""));
    if (old_t.empty()) return fail("old_string is empty");
    auto pos = text->find(old_t);
    if (pos == std::string::npos) return fail("old_string not found in " + path_display(path));
    if (text->find(old_t, pos + 1) != std::string::npos)
        return fail("old_string is not unique in " + path_display(path) +
                    " — provide more context");
    *text = text->substr(0, pos) + new_t + text->substr(pos + old_t.size());
    if (!write_file_text(path, *text)) return fail("Cannot write " + path_display(path));
    return ok("Edited " + path_display(path));
}

static json tool_list_dir(ToolContext& ctx, const json& args) {
    auto path = resolve_in_cwd(ctx, args.value("path", "."));
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return fail("Not a directory: " + path_display(path));
    std::string out;
    int n = 0;
    for (auto& e : fs::directory_iterator(path, ec)) {
        if (++n > 500) {
            out += "…\n";
            break;
        }
        std::string name = e.path().filename().string();
        if (e.is_directory()) out += name + "/\n";
        else {
            std::error_code ec2;
            out += name + "  (" + std::to_string(e.file_size(ec2)) + " B)\n";
        }
    }
    if (out.empty()) out = "(empty)";
    return ok(out);
}

// ---------- search ----------

static bool match_glob(const std::string& pattern, const std::string& name) {
    // supports *, **, ?
    std::string re;
    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                re += ".*";
                ++i;
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') ++i;
            } else {
                re += "[^/]*";
            }
        } else if (c == '?') re += "[^/]";
        else if (strchr("\\^$.|+()[]{}", c)) {
            re += '\\';
            re += c;
        } else re += c;
    }
    try {
        return std::regex_match(name, std::regex(re));
    } catch (...) {
        return name == pattern;
    }
}

static void walk_files(const fs::path& root, const fs::path& dir, int depth,
                       const std::function<bool(const fs::path&)>& pred,
                       std::vector<fs::path>& out, int limit) {
    if ((int)out.size() >= limit || depth > 12) return;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if ((int)out.size() >= limit) return;
        auto name = e.path().filename().string();
        if (name == ".git" || name == "node_modules" || name == "target" || name == "dist" ||
            name == "__pycache__" || name == ".venv" || name == "legacy")
            continue;
        if (e.is_directory()) {
            walk_files(root, e.path(), depth + 1, pred, out, limit);
        } else if (e.is_regular_file()) {
            if (!pred || pred(e.path())) out.push_back(e.path());
        }
    }
}

static json tool_glob(ToolContext& ctx, const json& args) {
    std::string pattern = args.value("pattern", "**/*");
    std::vector<fs::path> hits;
    walk_files(ctx.cwd, ctx.cwd, 0,
               [&](const fs::path& p) {
                   return match_glob(pattern, p.generic_string().substr(
                                                  ctx.cwd.generic_string().size() + 1));
               },
               hits, 200);
    std::string out;
    for (auto& p : hits) out += path_display(p) + "\n";
    if (out.empty()) out = "(no matches)";
    return ok(clamp_text(out));
}

static json tool_grep(ToolContext& ctx, const json& args) {
    std::string pattern = args.value("pattern", "");
    if (pattern.empty()) return fail("pattern is empty");
    std::string glob = args.value("glob", "**/*");
    std::vector<fs::path> files;
    walk_files(
        ctx.cwd, ctx.cwd, 0,
        [&](const fs::path& p) {
            return match_glob(glob, p.generic_string().substr(ctx.cwd.generic_string().size() + 1));
        },
        files, 2000);
    std::regex re;
    bool use_re = true;
    try {
        re = std::regex(pattern, std::regex::icase);
    } catch (...) {
        use_re = false;
    }
    std::string out;
    int hits = 0;
    for (auto& f : files) {
        if (hits >= 120) break;
        auto text = read_file_text(f);
        if (!text) continue;
        auto lines = split_lines(*text);
        auto rel = path_display(f);
        if (starts_with(rel, ctx.cwd.generic_string() + "/"))
            rel = rel.substr(ctx.cwd.generic_string().size() + 1);
        for (size_t i = 0; i < lines.size() && hits < 120; ++i) {
            bool m = use_re ? std::regex_search(lines[i], re)
                            : lines[i].find(pattern) != std::string::npos;
            if (m) {
                out += rel + ":" + std::to_string(i + 1) + ":" + lines[i] + "\n";
                ++hits;
            }
        }
    }
    if (out.empty()) out = "(no matches)";
    return ok(clamp_text(out));
}

// ---------- shell ----------

static json tool_shell(ToolContext& ctx, const json& args) {
    if (ctx.mode == AgentMode::Plan) return fail("plan mode: shell is disabled");
    std::string cmd = args.value("command", "");
    if (cmd.empty()) return fail("command is empty");
    int timeout_sec = std::clamp(args.value("timeout", 60), 1, 300);

#ifdef _WIN32
    // Run via cmd.exe, capture combined output to temp file.
    fs::path tmp = fs::temp_directory_path() /
                   ("sm_shell_" + std::to_string(GetTickCount64()) + ".txt");
    std::string full = "cmd /C (" + cmd + ") > \"" + path_display(tmp) + "\" 2>&1";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = utf8_to_wide(full);
    std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
    buf.push_back(0);
    BOOL created = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                  utf8_to_wide(path_display(ctx.cwd)).c_str(), &si, &pi);
    if (!created) return fail("CreateProcess failed");
    // wait with timeout
    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_sec * 1000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        fs::remove(tmp);
        return fail("command timed out after " + std::to_string(timeout_sec) + "s");
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    auto out = read_file_text(tmp).value_or("");
    fs::remove(tmp);
    std::string head = "exit=" + std::to_string(code) + "\n" + clamp_text(out, 30000);
    return ok(head);
#else
    fs::path tmp = fs::temp_directory_path() /
                   ("sm_shell_" + std::to_string((long long)getpid()) + ".txt");
    std::string full = "(" + cmd + ") > '" + path_display(tmp) + "' 2>&1";
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", full.c_str(), (char*)nullptr);
        _exit(127);
    }
    int status = 0;
    // simple wait with timeout via alarm-free poll loop
    for (int i = 0; i < timeout_sec * 10; ++i) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (i == timeout_sec * 10 - 1) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            fs::remove(tmp);
            return fail("command timed out");
        }
        usleep(100000);
    }
    auto out = read_file_text(tmp).value_or("");
    fs::remove(tmp);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return ok("exit=" + std::to_string(code) + "\n" + clamp_text(out, 30000));
#endif
}

// ---------- screen / cursor ----------

static json tool_screenshot(ToolContext& ctx, const json& args) {
    (void)args;
    auto jpg = capture_screen_jpeg(ctx.cfg ? ctx.cfg->max_side : 1600,
                                   ctx.cfg ? ctx.cfg->quality : 70,
                                   ctx.cfg ? ctx.cfg->region : std::vector<int>{});
    if (jpg.empty()) return fail("screenshot unavailable on this platform");
    ctx.last_screenshot_jpeg = jpg;
    json r = ok("Screenshot captured (" + std::to_string(jpg.size()) +
                " bytes JPEG). Image is attached to this tool result.");
    r["image_b64"] = base64_encode(jpg);
    return r;
}

static json tool_list_windows(ToolContext&, const json&) {
    auto wins = list_windows();
    if (wins.empty()) return ok("(no visible windows enumerated)");
    std::string out;
    int i = 0;
    for (auto& w : wins) {
        if (++i > 80) break;
        out += "- " + w.title;
        if (!w.process.empty()) out += "  [" + w.process + "]";
        out += "\n";
    }
    return ok(clamp_text(out));
}

static json tool_cursor(ToolContext& ctx, const json& args) {
    if (ctx.mode == AgentMode::Plan) return fail("plan mode: cursor is disabled");
    CursorAction a;
    a.kind = args.value("action", "MOVE");
    std::transform(a.kind.begin(), a.kind.end(), a.kind.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });
    a.x = args.value("x", 0);
    a.y = args.value("y", 0);
    a.x2 = args.value("x2", args.value("end_x", 0));
    a.y2 = args.value("y2", args.value("end_y", 0));
    a.dx = args.value("dx", args.value("notches", 0));
    a.dy = args.value("dy", 0);
    a.text = args.value("text", args.value("key", ""));
    auto clamp_pt = [&](int& x, int& y) {
        if (ctx.cfg && ctx.cfg->region.size() == 4) {
            int rx = ctx.cfg->region[0], ry = ctx.cfg->region[1];
            int rw = ctx.cfg->region[2], rh = ctx.cfg->region[3];
            x = std::clamp(x, rx, rx + rw - 1);
            y = std::clamp(y, ry, ry + rh - 1);
        }
    };
    clamp_pt(a.x, a.y);
    clamp_pt(a.x2, a.y2);
    return ok(cursor_perform(a));
}

// ---------- LSP ----------

static json tool_lsp(ToolContext& ctx, const json& args) {
    std::string op = args.value("op", "definition");
    auto path = resolve_in_cwd(ctx, args.value("path", ""));
    auto text = read_file_text(path);
    if (!text) return fail("Cannot read " + path_display(path));
    std::string cmd = lsp_command_for(ctx.cfg ? ctx.cfg->lsp_servers : std::map<std::string, std::string>{},
                                      path_display(path));
    if (cmd.empty())
        return fail("No LSP configured for extension of " + path_display(path) +
                    ". Add it to config.json lsp_servers, e.g. {\".py\":\"pylsp\"}.");
    LspServer& server = lsp_global_server();
    std::string err;
    if (!server.running()) {
        if (!server.start(cmd, &err)) return fail(err);
        server.initialize("file:///" + ctx.cwd.generic_string());
    }
    int line = args.value("line", 0);
    int character = args.value("character", 0);
    if (op == "hover") return ok(server.hover(path_display(path), *text, line, character));
    if (op == "open") {
        server.did_open(path_display(path), *text);
        return ok("opened");
    }
    return ok(server.definition(path_display(path), *text, line, character));
}

// ---------- schemas ----------

nlohmann::json tool_schemas(AgentMode mode) {
    auto prop = [](const char* name, const char* type, const char* desc) {
        return json{{name, {{"type", type}, {"description", desc}}}};
    };
    json schemas = json::array();

    auto add = [&](const char* name, const char* desc, json props,
                   std::vector<const char*> required) {
        json req = json::array();
        for (auto* r : required) req.push_back(r);
        schemas.push_back({{"type", "function"},
                           {"function",
                            {{name, name},
                             {"description", desc},
                             {"parameters", {{"type", "object"}, {"properties", props},
                                             {"required", req}}}}}});
    };

    add("read_file", "Read a text file from the project. Returns numbered lines.",
        json{{prop("path", "string", "file path relative to project root")},
             {"offset", {{"type", "integer"}, {"description", "byte offset"}}},
             {"limit", {{"type", "integer"}, {"description", "max bytes"}}}},
        {"path"});
    add("list_dir", "List directory contents.",
        json{{prop("path", "string", "directory (default .)")}}, {});
    add("glob", "Find files by glob pattern (**/*.cpp, src/*.hpp).",
        json{{prop("pattern", "string", "glob pattern")}}, {"pattern"});
    add("grep", "Search file contents by regex or literal substring.",
        json{{prop("pattern", "string", "regex or literal")},
             {"glob", {{"type", "string"}, {"description", "file filter, default **/*"}}}},
        {"pattern"});
    add("screenshot", "Capture the user's screen as an image (vision).", json::object(), {});
    add("list_windows", "List visible windows with titles and process names — use to identify "
                        "which programs are on screen.",
        json::object(), {});
    add("lsp", "Talk to a language server: definition / hover.",
        json{{prop("op", "string", "definition | hover | open")},
             {"path", {{"type", "string"}, {"description", "source file"}}},
             {"line", {{"type", "integer"}, {"description", "0-based line"}}},
             {"character", {{"type", "integer"}, {"description", "0-based column"}}}},
        {"op", "path"});

    if (mode == AgentMode::Build) {
        add("write_file", "Create or overwrite a file.",
            json{{prop("path", "string", "file path")},
                 {"content", {{"type", "string"}, {"description", "full file content"}}}},
            {"path", "content"});
        add("edit_file", "Replace an exact unique old_string with new_string in a file.",
            json{{prop("path", "string", "file path")},
                 {"old_string", {{"type", "string"}, {"description", "exact text to replace"}}},
                 {"new_string", {{"type", "string"}, {"description", "replacement"}}}},
            {"path", "old_string", "new_string"});
        add("shell", "Run a shell command in the project root (build mode only).",
            json{{prop("command", "string", "command line")},
                 {"timeout", {{"type", "integer"}, {"description", "seconds, default 60"}}}},
            {"command"});
        add("cursor",
            "Drive the real mouse/keyboard on the user's screen. "
            "Workflow: screenshot → find target pixels → act → screenshot to verify. "
            "Actions: MOVE/HOVER(x,y); CLICK/RIGHT_CLICK/MIDDLE_CLICK/DOUBLE_CLICK(x,y); "
            "DRAG(x,y to x2,y2); SCROLL(x,y,dx notches, +up/−down); "
            "KEY(text=enter|ctrl+s|f5…); TYPE(text); POS(). "
            "After MOVE the pointer stays there (hover works). Absolute screen pixels.",
            json{{prop("action", "string",
                       "MOVE | HOVER | CLICK | RIGHT_CLICK | MIDDLE_CLICK | "
                       "DOUBLE_CLICK | DRAG | SCROLL | KEY | TYPE | POS")},
                 {"x", {{"type", "integer"}, {"description", "screen x (DRAG start, SCROLL anchor)"}}},
                 {"y", {{"type", "integer"}, {"description", "screen y"}}},
                 {"x2", {{"type", "integer"}, {"description", "DRAG end x"}}},
                 {"y2", {{"type", "integer"}, {"description", "DRAG end y"}}},
                 {"dx", {{"type", "integer"}, {"description", "SCROLL notches (+up, −down)"}}},
                 {"text", {{"type", "string"}, {"description", "TYPE text or KEY name (enter, ctrl+s, esc…)"}}}},
            {"action"});
    }
    return schemas;
}

// ---------- dispatch ----------

nlohmann::json run_tool(ToolContext& ctx, const std::string& name, const nlohmann::json& args) {
    try {
        if (name == "read_file") return tool_read_file(ctx, args);
        if (name == "list_dir") return tool_list_dir(ctx, args);
        if (name == "glob") return tool_glob(ctx, args);
        if (name == "grep") return tool_grep(ctx, args);
        if (name == "screenshot") return tool_screenshot(ctx, args);
        if (name == "list_windows") return tool_list_windows(ctx, args);
        if (name == "lsp") return tool_lsp(ctx, args);
        if (name == "write_file") return tool_write_file(ctx, args);
        if (name == "edit_file") return tool_edit_file(ctx, args);
        if (name == "shell") return tool_shell(ctx, args);
        if (name == "cursor") return tool_cursor(ctx, args);
        return fail("unknown tool: " + name);
    } catch (const std::exception& e) {
        return fail(std::string("tool error: ") + e.what());
    }
}

std::vector<std::string> run_bracket_commands(const std::string& text, ToolContext& ctx) {
    // [MOVE x y] / [CLICK x y] / [RIGHT_CLICK x y] / [DOUBLE_CLICK x y] / [TYPE ...]
    static const std::regex re(
        R"(\[\s*(MOVE|HOVER|CLICK|RIGHT_CLICK|MIDDLE_CLICK|DOUBLE_CLICK|DRAG|SCROLL|KEY|TYPE|POS)\s*([^\]]*)\])",
        std::regex::icase);
    std::vector<std::string> results;
    auto begin = std::sregex_iterator(text.begin(), text.end(), re);
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        std::string kind = (*it)[1].str();
        std::transform(kind.begin(), kind.end(), kind.begin(),
                       [](unsigned char c) { return (char)std::toupper(c); });
        std::string rest = trim((*it)[2].str());
        CursorAction a;
        a.kind = kind;
        if (kind == "TYPE" || kind == "KEY") {
            a.text = rest;
        } else if (kind == "POS") {
            // no args
        } else if (kind == "SCROLL") {
            int x = 0, y = 0, n = 0;
            if (std::sscanf(rest.c_str(), "%d%*[, ]%d%*[, ]%d", &x, &y, &n) == 3) {
                a.x = x; a.y = y; a.dx = n;
            } else if (std::sscanf(rest.c_str(), "%d", &n) == 1) {
                a.dx = n;
            } else {
                continue;
            }
        } else if (kind == "DRAG") {
            int x = 0, y = 0, x2 = 0, y2 = 0;
            if (std::sscanf(rest.c_str(), "%d%*[, ]%d%*[, ]%d%*[, ]%d",
                            &x, &y, &x2, &y2) == 4) {
                a.x = x; a.y = y; a.x2 = x2; a.y2 = y2;
            } else {
                continue;
            }
        } else {
            int x = 0, y = 0;
            if (std::sscanf(rest.c_str(), "%d%*[, ]%d", &x, &y) == 2) {
                a.x = x;
                a.y = y;
            } else {
                continue;
            }
        }
        if (ctx.mode == AgentMode::Plan) continue;
        results.push_back(cursor_perform(a));
    }
    return results;
}

}  // namespace sm
