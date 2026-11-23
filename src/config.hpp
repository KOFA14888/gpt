#pragma once
// Settings for ScreenMate AI — stored as JSON under %APPDATA% (Windows)
// or ~/.config (Linux/macOS). Keys may also come from environment / .env.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "common.hpp"
#include <nlohmann/json.hpp>

namespace sm {

inline constexpr const char* kAppName = "ScreenMate AI";

inline const char* kDefaultSystemPrompt =
    "Ты — ScreenMate AI, CLI-кодинг-агент с доступом к экрану пользователя. "
    "Отвечай на русском языке, коротко и по делу. "
    "Ты работаешь в репозитории текущей директории: читай и правь файлы, "
    "запускай команды, ищи по коду. "
    "Если пользователь спрашивает про экран — используй list_windows и screenshot: "
    "на скриншоте видны кнопки, меню, ошибки, текст. Не выдумывай то, чего не видно. "
    "Планируй коротко перед правками; не ломай то, что не просили.";

struct Config {
    std::string provider = "nvidia";  // "nvidia" | "openrouter"
    std::string openrouter_key;
    std::string nvidia_key;
    std::string openrouter_model = "google/gemma-4-31b-it:free";
    std::string nvidia_model = "nvidia/nemotron-3.5-super-vl-120b-a12b";
    std::string system_prompt = kDefaultSystemPrompt;
    bool attach_screenshot = true;
    int max_side = 1600;
    int quality = 70;
    int history_limit = 16;
    std::vector<int> region;  // optional [x,y,w,h]
    // extension -> language server command (e.g. ".py" -> "pylsp")
    std::map<std::string, std::string> lsp_servers;

    std::string api_key() const {
        return provider == "nvidia" ? nvidia_key : openrouter_key;
    }
    std::string model() const {
        return provider == "nvidia" ? nvidia_model : openrouter_model;
    }
    std::string chat_url() const {
        return provider == "nvidia"
                   ? "https://integrate.api.nvidia.com/v1/chat/completions"
                   : "https://openrouter.ai/api/v1/chat/completions";
    }

    static fs::path config_dir() {
#ifdef _WIN32
        const char* base = std::getenv("APPDATA");
        fs::path p = base ? fs::path(base) : fs::path(".");
        return p / "screenmate_ai";
#else
        const char* base = std::getenv("XDG_CONFIG_HOME");
        fs::path p = base ? fs::path(base) : (fs::path(getenv("HOME") ? getenv("HOME") : ".") / ".config");
        return p / "screenmate_ai";
#endif
    }

    static fs::path config_file() { return config_dir() / "config.json"; }

    static std::string read_env_file(const fs::path& env_path) {
        // minimal KEY=VALUE lines
        std::ifstream in(env_path);
        if (!in) return "";
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(line.substr(0, eq));
            std::string v = trim(line.substr(eq + 1));
            if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                                  (v.front() == '\'' && v.back() == '\''))) {
                v = v.substr(1, v.size() - 2);
            }
            if (k == "OPENROUTER_API_KEY" || k == "NVIDIA_NIM_API_KEY" || k == "NVIDIA_API_KEY" ||
                k == "OPENROUTER_MODEL" || k == "NVIDIA_MODEL") {
                if (!v.empty()) return k + "=" + v;
            }
        }
        return "";
    }

    static Config load() {
        Config c;
        try {
            std::ifstream in(config_file());
            if (in) {
                nlohmann::json j;
                in >> j;
                if (j.contains("provider")) c.provider = j["provider"].get<std::string>();
                if (j.contains("openrouter_key")) c.openrouter_key = j["openrouter_key"].get<std::string>();
                if (j.contains("nvidia_key")) c.nvidia_key = j["nvidia_key"].get<std::string>();
                if (j.contains("openrouter_model")) c.openrouter_model = j["openrouter_model"].get<std::string>();
                if (j.contains("nvidia_model")) c.nvidia_model = j["nvidia_model"].get<std::string>();
                if (j.contains("system_prompt")) c.system_prompt = j["system_prompt"].get<std::string>();
                if (j.contains("attach_screenshot")) c.attach_screenshot = j["attach_screenshot"].get<bool>();
                if (j.contains("max_side")) c.max_side = j["max_side"].get<int>();
                if (j.contains("quality")) c.quality = j["quality"].get<int>();
                if (j.contains("history_limit")) c.history_limit = j["history_limit"].get<int>();
                if (j.contains("region") && j["region"].is_array()) {
                    c.region.clear();
                    for (auto& v : j["region"]) c.region.push_back(v.get<int>());
                }
                if (j.contains("lsp_servers") && j["lsp_servers"].is_object()) {
                    for (auto it = j["lsp_servers"].begin(); it != j["lsp_servers"].end(); ++it)
                        c.lsp_servers[it.key()] = it.value().get<std::string>();
                }
            }
        } catch (...) {
        }

        // .env next to binary/cwd, then process env
        auto apply_kv = [&](const std::string& kv) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) return;
            std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "OPENROUTER_API_KEY") c.openrouter_key = v;
            else if (k == "NVIDIA_NIM_API_KEY" || k == "NVIDIA_API_KEY") c.nvidia_key = v;
            else if (k == "OPENROUTER_MODEL") c.openrouter_model = v;
            else if (k == "NVIDIA_MODEL") c.nvidia_model = v;
        };
        apply_kv(read_env_file(fs::path(".env")));
        if (const char* v = std::getenv("OPENROUTER_API_KEY")) c.openrouter_key = v;
        if (const char* v = std::getenv("NVIDIA_NIM_API_KEY")) c.nvidia_key = v;
        if (const char* v = std::getenv("NVIDIA_API_KEY")) c.nvidia_key = v;

        if (c.provider != "nvidia" && c.provider != "openrouter") c.provider = "openrouter";
        if (c.system_prompt.empty()) c.system_prompt = kDefaultSystemPrompt;
        c.max_side = std::clamp(c.max_side, 640, 2560);
        c.quality = std::clamp(c.quality, 35, 95);
        c.history_limit = std::clamp(c.history_limit, 0, 64);
        return c;
    }

    void save() const {
        std::error_code ec;
        fs::create_directories(config_dir(), ec);
        nlohmann::json j;
        j["provider"] = provider;
        j["openrouter_key"] = openrouter_key;
        j["nvidia_key"] = nvidia_key;
        j["openrouter_model"] = openrouter_model;
        j["nvidia_model"] = nvidia_model;
        j["system_prompt"] = system_prompt;
        j["attach_screenshot"] = attach_screenshot;
        j["max_side"] = max_side;
        j["quality"] = quality;
        j["history_limit"] = history_limit;
        j["region"] = region;
        j["lsp_servers"] = lsp_servers;
        std::ofstream out(config_file(), std::ios::trunc);
        out << j.dump(2);
    }
};

}  // namespace sm
