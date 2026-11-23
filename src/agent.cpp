#include "agent.hpp"

#include "common.hpp"
#include "http.hpp"
#include "screen.hpp"

#include <stdexcept>

namespace sm {

using json = nlohmann::json;

void StreamAccum::feed(const std::string& data) {
    if (data == "[DONE]") {
        if (finish_reason.empty()) finish_reason = "stop";
        return;
    }
    json ev;
    try {
        ev = json::parse(data);
    } catch (...) {
        return;
    }
    if (ev.contains("error")) {
        std::string msg = "provider error";
        if (ev["error"].is_object())
            msg = ev["error"].value("message", msg);
        else if (ev["error"].is_string())
            msg = ev["error"].get<std::string>();
        throw std::runtime_error(msg);
    }
    if (!ev.contains("choices") || ev["choices"].empty()) return;
    const auto& ch = ev["choices"][0];
    if (ch.contains("finish_reason") && !ch["finish_reason"].is_null())
        finish_reason = ch["finish_reason"].get<std::string>();
    if (!ch.contains("delta")) return;
    const auto& delta = ch["delta"];
    if (delta.contains("content") && delta["content"].is_string())
        text += delta["content"].get<std::string>();
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& tc : delta["tool_calls"]) {
            int idx = tc.value("index", 0);
            auto& slot = tool_calls[idx];
            if (tc.contains("id") && tc["id"].is_string() && !tc["id"].get<std::string>().empty())
                slot["id"] = tc["id"].get<std::string>();
            if (tc.contains("function")) {
                const auto& f = tc["function"];
                if (f.contains("name") && f["name"].is_string() && !f["name"].get<std::string>().empty())
                    slot["name"] = f["name"].get<std::string>();
                if (f.contains("arguments") && f["arguments"].is_string())
                    slot["args"] = slot.value("args", std::string()) +
                                   f["arguments"].get<std::string>();
            }
            if (slot.contains("id") == false && tc.contains("id")) slot["id"] = tc["id"];
        }
    }
}

nlohmann::json build_messages(Config& cfg, const std::vector<nlohmann::json>& history,
                              const std::string& extra_system, bool attach_shot) {
    json msgs = json::array();
    std::string sys = cfg.system_prompt;
    if (!extra_system.empty()) sys += "\n\n" + extra_system;
    msgs.push_back({{"role", "system"}, {"content", sys}});
    for (auto& m : history) msgs.push_back(m);
    (void)attach_shot;
    return msgs;
}

static json make_user_with_shot(const std::string& text, const std::string& jpeg_b64) {
    if (jpeg_b64.empty()) return {{"role", "user"}, {"content", text}};
    json parts = json::array();
    parts.push_back({{"type", "text"}, {"text", text}});
    parts.push_back(
        {{"type", "image_url"},
         {"image_url", {{"url", "data:image/jpeg;base64," + jpeg_b64}}}});
    return {{"role", "user"}, {"content", parts}};
}

static std::string http_error_body(const std::string& e) {
    // HttpError message already contains HTTP status + body snippet
    return e;
}

std::string agent_turn(Config& cfg, ToolContext& ctx, std::vector<json>& history,
                       const std::string& user_text, const AgentCallbacks& cb,
                       const std::string& mode_label) {
    if (cfg.api_key().empty())
        throw std::runtime_error("No API key. Run: screenmate auth login");

    std::string shot_b64;
    if (cfg.attach_screenshot) {
        auto jpg = capture_screen_jpeg(cfg.max_side, cfg.quality, cfg.region);
        if (!jpg.empty()) shot_b64 = base64_encode(jpg);
    }
    history.push_back(make_user_with_shot(user_text, shot_b64));

    std::string mode_sys =
        "Режим агента: " + mode_label + ". " +
        (ctx.mode == AgentMode::Plan
             ? "PLAN — только чтение и анализ: write_file/edit_file/shell/cursor запрещены."
             : "BUILD — полный доступ: можно править файлы, запускать команды и двигать курсор.");

    json tools = tool_schemas(ctx.mode);
    std::string final_text;

    for (int step = 0; step < 40; ++step) {
        json messages = build_messages(cfg, history, mode_sys, true);

        json payload = {
            {"model", cfg.model()},
            {"messages", messages},
            {"stream", true},
            {"temperature", 0.3},
            {"max_tokens", 4096},
        };
        if (!tools.empty()) payload["tools"] = tools;

        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + cfg.api_key()},
            {"Content-Type", "application/json"},
            {"Accept", "text/event-stream"},
        };
        if (cfg.provider == "openrouter") {
            headers["HTTP-Referer"] = "https://github.com/KOFA14888/gpt";
            headers["X-Title"] = "ScreenMate AI";
        }

        StreamAccum acc;
        std::string raw = payload.dump();
        std::string sse_buf;

        auto on_sse = [&](const std::string& chunk) {
            sse_buf += chunk;
            size_t pos;
            while ((pos = sse_buf.find('\n')) != std::string::npos) {
                std::string line = sse_buf.substr(0, pos);
                sse_buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!starts_with(line, "data:")) continue;
                std::string data = trim(line.substr(5));
                if (data.empty()) continue;
                size_t before = acc.text.size();
                acc.feed(data);
                if (cb.on_text && acc.text.size() > before)
                    cb.on_text(acc.text.substr(before));
            }
        };

        try {
            http_post_sse(cfg.chat_url(), headers, raw, on_sse, 30, 600);
        } catch (const std::exception& e) {
            throw std::runtime_error(e.what());
        }

        if (acc.text.empty() && !acc.has_tools())
            throw std::runtime_error(
                "Model returned an empty response. Try another model (screenmate auth login).");

        if (acc.has_tools()) {
            json content = acc.text.empty() ? json(nullptr) : json(acc.text);
            json assistant = {{"role", "assistant"}, {"content", content}};
            json tcs = json::array();
            std::vector<int> idxs;
            for (auto& kv : acc.tool_calls) idxs.push_back(kv.first);
            std::sort(idxs.begin(), idxs.end());
            for (int i : idxs) {
                auto& tc = acc.tool_calls[i];
                std::string id = tc.value("id", "call_" + std::to_string(i));
                std::string name = tc.value("name", "");
                std::string args_s = tc.value("args", std::string("{}"));
                if (args_s.empty()) args_s = "{}";
                tcs.push_back({{"id", id},
                               {"type", "function"},
                               {"function", {{"name", name}, {"arguments", args_s}}}});
                if (cb.on_status) {
                    std::string pretty = args_s;
                    if (pretty.size() > 120) pretty = pretty.substr(0, 120) + "…";
                    cb.on_status("→ " + name + " " + pretty);
                }
            }
            assistant["tool_calls"] = tcs;
            history.push_back(assistant);

            for (auto& tc : tcs) {
                std::string id = tc["id"].get<std::string>();
                std::string name = tc["function"]["name"].get<std::string>();
                json args = json::object();
                try {
                    args = json::parse(tc["function"]["arguments"].get<std::string>());
                } catch (...) {
                }
                json result = run_tool(ctx, name, args);
                std::string content_s = result.value("content", "");
                json tool_msg = {{"role", "tool"},
                                 {"tool_call_id", id},
                                 {"content", content_s}};
                if (result.contains("image_b64")) {
                    json parts = json::array();
                    parts.push_back({{"type", "text"}, {"text", content_s}});
                    parts.push_back({{"type", "image_url"},
                                     {"image_url",
                                      {{"url", "data:image/jpeg;base64," +
                                                   result["image_b64"].get<std::string>()}}}});
                    tool_msg["content"] = parts;
                    if (cb.on_image) cb.on_image(result["image_b64"].get<std::string>());
                }
                history.push_back(tool_msg);
            }
            continue;
        }

        final_text = acc.text;
        json assistant = {{"role", "assistant"}, {"content", final_text}};
        history.push_back(assistant);
        if (ctx.mode == AgentMode::Build) {
            auto cmds = run_bracket_commands(final_text, ctx);
            if (!cmds.empty() && cb.on_status) {
                for (auto& c : cmds) cb.on_status("• " + c);
            }
        }
        if (cfg.history_limit > 0 && (int)history.size() > cfg.history_limit * 2) {
            size_t drop = history.size() - (size_t)cfg.history_limit * 2;
            history.erase(history.begin(), history.begin() + drop);
        }
        return final_text;
    }
    throw std::runtime_error("Agent stopped: too many tool steps");
}

}  // namespace sm
