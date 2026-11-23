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
#include <vector>

#include "common.hpp"
#include <nlohmann/json.hpp>

namespace sm {

inline constexpr const char* kAppName = "ScreenMate AI";

inline const char* kDefaultSystemPrompt =
    "Ты — ScreenMate AI, консольный кодинг-агент в духе Claude Code. "
    "Отвечай на русском языке, коротко и по делу. "
    "Ты работаешь в репозитории текущей директории: читай и правь файлы, "
    "запускай команды, ищи по коду, используй LSP. "
    "Планируй коротко перед правками; не ломай то, что не просили.";

// One LLM provider. style selects wire protocol:
//   "openai"    — POST {base}/chat/completions, Bearer key (OpenAI-compatible)
//   "anthropic" — POST {base}/v1/messages, x-api-key
//   "gemini"    — POST {base}/models/{model}:streamGenerateContent?alt=sse, x-goog-api-key
struct ProviderDef {
    std::string id;
    std::string label;
    std::string style;
    std::string base;   // API base URL (no trailing slash)
    std::string env;    // primary env var for the key
    std::string model;  // default model id
};

inline const std::vector<ProviderDef>& provider_defs() {
    static const std::vector<ProviderDef> v = {
        {"nvidia", "NVIDIA NIM", "openai", "https://integrate.api.nvidia.com/v1",
         "NVIDIA_NIM_API_KEY", "nvidia/nemotron-3.5-super-vl-120b-a12b"},
        {"openrouter", "OpenRouter", "openai", "https://openrouter.ai/api/v1",
         "OPENROUTER_API_KEY", "google/gemma-4-31b-it:free"},
        {"openai", "OpenAI", "openai", "https://api.openai.com/v1",
         "OPENAI_API_KEY", "gpt-4o-mini"},
        {"anthropic", "Anthropic (Claude)", "anthropic", "https://api.anthropic.com",
         "ANTHROPIC_API_KEY", "claude-sonnet-4-5"},
        {"gemini", "Google Gemini", "gemini",
         "https://generativelanguage.googleapis.com/v1beta", "GEMINI_API_KEY",
         "gemini-2.5-flash"},
        {"groq", "Groq", "openai", "https://api.groq.com/openai/v1",
         "GROQ_API_KEY", "llama-3.3-70b-versatile"},
        {"deepseek", "DeepSeek", "openai", "https://api.deepseek.com/v1",
         "DEEPSEEK_API_KEY", "deepseek-chat"},
        {"mistral", "Mistral", "openai", "https://api.mistral.ai/v1",
         "MISTRAL_API_KEY", "mistral-large-latest"},
        {"together", "Together AI", "openai", "https://api.together.xyz/v1",
         "TOGETHER_API_KEY", "meta-llama/Llama-3.3-70B-Instruct-Turbo"},
        {"custom", "Custom (OpenAI-compatible)", "openai", "",
         "CUSTOM_API_KEY", ""},
    };
    return v;
}

inline const ProviderDef* find_provider(const std::string& id) {
    for (const auto& p : provider_defs())
        if (p.id == id) return &p;
    return nullptr;
}

struct Config {
    std::string provider = "nvidia";
    // provider id -> API key
    std::map<std::string, std::string> keys;
    // provider id -> model id (empty = provider default)
    std::map<std::string, std::string> models;
    // base URL for provider == "custom"
    std::string custom_base_url;
    std::string system_prompt = kDefaultSystemPrompt;
    int history_limit = 16;
    // extension -> language server command (e.g. ".py" -> "pylsp")
    std::map<std::string, std::string> lsp_servers;

    const ProviderDef* def() const {
        const ProviderDef* d = find_provider(provider);
        return d ? d : find_provider("openrouter");
    }

    std::string style() const {
        const ProviderDef* d = def();
        return d ? d->style : "openai";
    }

    std::string api_key() const {
        auto it = keys.find(provider);
        return it == keys.end() ? std::string() : it->second;
    }

    bool has_key(const std::string& id) const {
        auto it = keys.find(id);
        return it != keys.end() && !it->second.empty();
    }

    std::string model() const {
        auto it = models.find(provider);
        if (it != models.end() && !it->second.empty()) return it->second;
        const ProviderDef* d = def();
        return d ? d->model : std::string();
    }

    std::string base_url() const {
        if (provider == "custom") return custom_base_url;
        const ProviderDef* d = def();
        return d ? d->base : std::string();
    }

    std::string chat_url() const {
        std::string st = style();
        if (st == "anthropic") return base_url() + "/v1/messages";
        if (st == "gemini") {
            std::string m = model();
            if (m.rfind("models/", 0) == 0) m = m.substr(7);
            return base_url() + "/models/" + m + ":streamGenerateContent?alt=sse";
        }
        std::string b = base_url();
        if (b.empty()) return "";
        if (provider == "custom" &&
            b.find("/chat/completions") != std::string::npos)
            return b;
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b + "/chat/completions";
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

    static std::vector<std::string> read_env_file(const fs::path& env_path) {
        // minimal KEY=VALUE lines — all recognized KVs
        std::vector<std::string> out;
        std::ifstream in(env_path);
        if (!in) return out;
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
            if (v.empty()) continue;
            bool key_like = k.size() > 4 && k.compare(k.size() - 4, 4, "_KEY") == 0;
            bool model_like = k.size() > 6 && k.compare(k.size() - 6, 6, "_MODEL") == 0;
            if (key_like || model_like) out.push_back(k + "=" + v);
        }
        return out;
    }

    static Config load() {
        Config c;
        try {
            std::ifstream in(config_file());
            if (in) {
                nlohmann::json j;
                in >> j;
                if (j.contains("provider")) c.provider = j["provider"].get<std::string>();
                // new format: per-provider maps
                if (j.contains("keys") && j["keys"].is_object()) {
                    for (auto it = j["keys"].begin(); it != j["keys"].end(); ++it)
                        c.keys[it.key()] = it.value().get<std::string>();
                } else {
                    // legacy flat fields
                    if (j.contains("openrouter_key"))
                        c.keys["openrouter"] = j["openrouter_key"].get<std::string>();
                    if (j.contains("nvidia_key"))
                        c.keys["nvidia"] = j["nvidia_key"].get<std::string>();
                }
                if (j.contains("models") && j["models"].is_object()) {
                    for (auto it = j["models"].begin(); it != j["models"].end(); ++it)
                        c.models[it.key()] = it.value().get<std::string>();
                } else {
                    if (j.contains("openrouter_model"))
                        c.models["openrouter"] = j["openrouter_model"].get<std::string>();
                    if (j.contains("nvidia_model"))
                        c.models["nvidia"] = j["nvidia_model"].get<std::string>();
                }
                if (j.contains("custom_base_url"))
                    c.custom_base_url = j["custom_base_url"].get<std::string>();
                if (j.contains("system_prompt")) c.system_prompt = j["system_prompt"].get<std::string>();
                if (j.contains("history_limit")) c.history_limit = j["history_limit"].get<int>();
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
            if (v.empty()) return;
            if (k.size() > 6 && k.compare(k.size() - 6, 6, "_MODEL") == 0) {
                // OPENROUTER_MODEL / NVIDIA_MODEL / OPENAI_MODEL / ...
                std::string pid = k.substr(0, k.size() - 6);
                if (pid == "NVIDIA") pid = "nvidia";
                else if (pid == "OPENROUTER") pid = "openrouter";
                else pid = to_lower(pid);
                if (find_provider(pid)) c.models[pid] = v;
                return;
            }
            for (const auto& d : provider_defs()) {
                if (k == d.env) {
                    c.keys[d.id] = v;
                    return;
                }
            }
            if (k == "NVIDIA_API_KEY") c.keys["nvidia"] = v;
            else if (k == "GOOGLE_API_KEY" || k == "GEMINI_API_KEY") c.keys["gemini"] = v;
        };
        for (const auto& kv : read_env_file(fs::path(".env"))) apply_kv(kv);
        for (const auto& d : provider_defs()) {
            if (const char* v = std::getenv(d.env.c_str()))
                if (v && *v) c.keys[d.id] = v;
        }
        if (const char* v = std::getenv("NVIDIA_API_KEY")) c.keys["nvidia"] = v;
        if (const char* v = std::getenv("GOOGLE_API_KEY")) c.keys["gemini"] = v;
        if (const char* v = std::getenv("GEMINI_API_KEY")) c.keys["gemini"] = v;

        if (!find_provider(c.provider)) c.provider = "openrouter";
        if (c.system_prompt.empty()) c.system_prompt = kDefaultSystemPrompt;
        c.history_limit = std::clamp(c.history_limit, 0, 64);
        return c;
    }

    void save() const {
        std::error_code ec;
        fs::create_directories(config_dir(), ec);
        nlohmann::json j;
        j["provider"] = provider;
        j["keys"] = keys;
        j["models"] = models;
        j["custom_base_url"] = custom_base_url;
        j["system_prompt"] = system_prompt;
        j["history_limit"] = history_limit;
        j["lsp_servers"] = lsp_servers;
        std::ofstream out(config_file(), std::ios::trunc);
        out << j.dump(2);
    }
};

}  // namespace sm
