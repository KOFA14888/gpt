#include "agent.hpp"

#include "common.hpp"
#include "http.hpp"
#include "screen.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace sm {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static std::string data_url_b64(const std::string& url) {
    auto pos = url.find("base64,");
    if (pos == std::string::npos) return "";
    return url.substr(pos + 7);
}

static std::string mime_from_url(const std::string& url) {
    if (url.rfind("data:image/png", 0) == 0) return "image/png";
    if (url.rfind("data:image/webp", 0) == 0) return "image/webp";
    if (url.rfind("data:image/gif", 0) == 0) return "image/gif";
    return "image/jpeg";
}

static json parse_args_or_empty(const std::string& s) {
    try {
        return json::parse(s.empty() ? "{}" : s);
    } catch (...) {
        return json::object();
    }
}

// ---------------------------------------------------------------------------
// stream accumulation (per wire style)
// ---------------------------------------------------------------------------

static void feed_openai(StreamAccum& a, const std::string& data) {
    if (data == "[DONE]") {
        if (a.finish_reason.empty()) a.finish_reason = "stop";
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
        a.finish_reason = ch["finish_reason"].get<std::string>();
    if (!ch.contains("delta")) return;
    const auto& delta = ch["delta"];
    if (delta.contains("content") && delta["content"].is_string())
        a.text += delta["content"].get<std::string>();
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& tc : delta["tool_calls"]) {
            int idx = tc.value("index", 0);
            auto& slot = a.tool_calls[idx];
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

static void feed_anthropic(StreamAccum& a, const std::string& data) {
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
    std::string type = ev.value("type", "");
    if (type == "content_block_start") {
        int idx = ev.value("index", 0);
        if (ev.contains("content_block")) {
            const auto& cb = ev["content_block"];
            if (cb.value("type", "") == "tool_use") {
                auto& slot = a.tool_calls[idx];
                slot["id"] = cb.value("id", "");
                slot["name"] = cb.value("name", "");
                slot["args"] = std::string();
            }
        }
    } else if (type == "content_block_delta") {
        int idx = ev.value("index", 0);
        if (!ev.contains("delta")) return;
        const auto& d = ev["delta"];
        std::string dt = d.value("type", "");
        if (dt == "text_delta") {
            a.text += d.value("text", "");
        } else if (dt == "input_json_delta") {
            auto it = a.tool_calls.find(idx);
            if (it == a.tool_calls.end()) {
                auto& slot = a.tool_calls[idx];
                slot["args"] = d.value("partial_json", std::string());
            } else {
                it->second["args"] =
                    it->second.value("args", std::string()) + d.value("partial_json", std::string());
            }
        }
    } else if (type == "message_delta") {
        if (ev.contains("delta")) {
            std::string sr = ev["delta"].value("stop_reason", "");
            if (!sr.empty()) a.finish_reason = sr;
        }
    } else if (type == "message_stop") {
        if (a.finish_reason.empty()) a.finish_reason = "stop";
    }
}

static void feed_gemini(StreamAccum& a, const std::string& data) {
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
    if (ev.contains("promptFeedback") && ev["promptFeedback"].contains("blockReason")) {
        throw std::runtime_error("Gemini blocked the prompt: " +
                                 ev["promptFeedback"]["blockReason"].get<std::string>());
    }
    if (!ev.contains("candidates") || ev["candidates"].empty()) return;
    const auto& cand = ev["candidates"][0];
    if (cand.contains("content") && cand["content"].contains("parts")) {
        for (const auto& p : cand["content"]["parts"]) {
            if (p.contains("text") && p["text"].is_string())
                a.text += p["text"].get<std::string>();
            if (p.contains("functionCall")) {
                int idx = (int)a.tool_calls.size();
                const auto& fc = p["functionCall"];
                auto& slot = a.tool_calls[idx];
                slot["id"] = "call_" + std::to_string(idx);
                slot["name"] = fc.value("name", "");
                json args = fc.value("args", json::object());
                slot["args"] = args.dump();
            }
        }
    }
    if (cand.contains("finishReason") && cand["finishReason"].is_string()) {
        std::string fr = cand["finishReason"].get<std::string>();
        if (fr == "STOP") a.finish_reason = "stop";
        else if (fr == "MAX_TOKENS") a.finish_reason = "length";
        else a.finish_reason = fr;
    }
}

void StreamAccum::feed(const std::string& data) {
    if (style == "anthropic") return feed_anthropic(*this, data);
    if (style == "gemini") return feed_gemini(*this, data);
    return feed_openai(*this, data);
}

// ---------------------------------------------------------------------------
// history conversion (OpenAI-style canonical -> wire format)
// ---------------------------------------------------------------------------

static json anthropic_blocks_from_content(const json& content) {
    json blocks = json::array();
    if (content.is_string()) {
        if (!content.empty()) blocks.push_back({{"type", "text"}, {"text", content}});
        return blocks;
    }
    if (content.is_array()) {
        for (const auto& p : content) {
            if (!p.is_object()) continue;
            std::string t = p.value("type", "");
            if (t == "text") {
                blocks.push_back({{"type", "text"}, {"text", p.value("text", "")}});
            } else if (t == "image_url") {
                std::string url = p["image_url"].value("url", "");
                blocks.push_back({{"type", "image"},
                                  {"source", {{"type", "base64"},
                                              {"media_type", mime_from_url(url)},
                                              {"data", data_url_b64(url)}}}});
            }
        }
    }
    return blocks;
}

static json convert_anthropic(const std::vector<json>& history) {
    json msgs = json::array();
    for (const auto& m : history) {
        std::string role = m.value("role", "");
        json entry;
        if (role == "user") {
            json blocks = anthropic_blocks_from_content(m.value("content", json("")));
            if (blocks.empty()) blocks.push_back({{"type", "text"}, {"text", ""}});
            entry = {{"role", "user"}, {"content", blocks}};
        } else if (role == "assistant") {
            json blocks = json::array();
            json content = m.value("content", json());
            if (content.is_string() && !content.empty())
                blocks.push_back({{"type", "text"}, {"text", content}});
            else if (content.is_array()) {
                for (const auto& p : content)
                    if (p.value("type", "") == "text")
                        blocks.push_back({{"type", "text"}, {"text", p.value("text", "")}});
            }
            if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                for (const auto& tc : m["tool_calls"]) {
                    blocks.push_back({{"type", "tool_use"},
                                      {"id", tc.value("id", "")},
                                      {"name", tc["function"].value("name", "")},
                                      {"input", parse_args_or_empty(
                                                   tc["function"].value("arguments", "{}"))}});
                }
            }
            if (blocks.empty()) blocks.push_back({{"type", "text"}, {"text", ""}});
            entry = {{"role", "assistant"}, {"content", blocks}};
        } else if (role == "tool") {
            json inner = anthropic_blocks_from_content(m.value("content", json("")));
            json tr = {{"type", "tool_result"},
                       {"tool_use_id", m.value("tool_call_id", "")}};
            if (inner.empty())
                tr["content"] = "";
            else
                tr["content"] = inner;
            entry = {{"role", "user"}, {"content", json::array({tr})}};
        } else {
            continue;
        }
        // merge consecutive same-role messages (anthropic prefers alternation)
        if (!msgs.empty() && msgs.back()["role"] == entry["role"] &&
            msgs.back()["content"].is_array() && entry["content"].is_array()) {
            for (auto& b : entry["content"]) msgs.back()["content"].push_back(b);
        } else {
            msgs.push_back(entry);
        }
    }
    return msgs;
}

static json convert_gemini(const std::vector<json>& history) {
    // tool_call_id -> function name (functionResponse needs the name)
    std::map<std::string, std::string> id2name;
    for (const auto& m : history) {
        if (m.value("role", "") != "assistant") continue;
        if (!m.contains("tool_calls") || !m["tool_calls"].is_array()) continue;
        for (const auto& tc : m["tool_calls"])
            id2name[tc.value("id", "")] = tc["function"].value("name", "");
    }

    auto parts_from_content = [&](const json& content) {
        json parts = json::array();
        if (content.is_string()) {
            if (!content.empty()) parts.push_back({{"text", content}});
            return parts;
        }
        if (content.is_array()) {
            for (const auto& p : content) {
                std::string t = p.value("type", "");
                if (t == "text") {
                    parts.push_back({{"text", p.value("text", "")}});
                } else if (t == "image_url") {
                    std::string url = p["image_url"].value("url", "");
                    parts.push_back({{"inline_data",
                                      {{"mime_type", mime_from_url(url)},
                                       {"data", data_url_b64(url)}}}});
                }
            }
        }
        return parts;
    };

    json contents = json::array();
    for (const auto& m : history) {
        std::string role = m.value("role", "");
        if (role == "user") {
            json parts = parts_from_content(m.value("content", json("")));
            if (parts.empty()) parts.push_back({{"text", ""}});
            contents.push_back({{"role", "user"}, {"parts", parts}});
        } else if (role == "assistant") {
            json parts = json::array();
            json content = m.value("content", json());
            if (content.is_string() && !content.empty())
                parts.push_back({{"text", content}});
            else if (content.is_array()) {
                for (const auto& p : content)
                    if (p.value("type", "") == "text")
                        parts.push_back({{"text", p.value("text", "")}});
            }
            if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                for (const auto& tc : m["tool_calls"]) {
                    parts.push_back({{"functionCall",
                                      {{ "name", tc["function"].value("name", "") },
                                       { "args", parse_args_or_empty(
                                                     tc["function"].value("arguments", "{}")) }}}});
                }
            }
            if (parts.empty()) parts.push_back({{"text", ""}});
            contents.push_back({{"role", "model"}, {"parts", parts}});
        } else if (role == "tool") {
            std::string id = m.value("tool_call_id", "");
            std::string name = id2name.count(id) ? id2name[id] : std::string("unknown");
            json content = m.value("content", json(""));
            std::string text;
            json img = nullptr;
            if (content.is_string()) {
                text = content;
            } else if (content.is_array()) {
                for (const auto& p : content) {
                    if (p.value("type", "") == "text") text += p.value("text", "");
                    else if (p.value("type", "") == "image_url") {
                        std::string url = p["image_url"].value("url", "");
                        img = {{"inline_data",
                                {{"mime_type", mime_from_url(url)},
                                 {"data", data_url_b64(url)}}}};
                    }
                }
            }
            json parts = json::array();
            parts.push_back({{"functionResponse",
                              {{ "name", name }, { "response", {{ "content", text }}}}}});
            if (img.is_object()) parts.push_back(img);
            contents.push_back({{"role", "user"}, {"parts", parts}});
        }
    }
    return contents;
}

static json convert_tools(const std::string& style, const json& openai_tools) {
    if (style == "openai" || openai_tools.empty()) return openai_tools;
    json out = json::array();
    if (style == "anthropic") {
        for (const auto& t : openai_tools) {
            const auto& f = t["function"];
            out.push_back({{"name", f.value("name", "")},
                           {"description", f.value("description", "")},
                           {"input_schema", f.value("parameters", json::object())}});
        }
    } else if (style == "gemini") {
        json decls = json::array();
        for (const auto& t : openai_tools) {
            const auto& f = t["function"];
            decls.push_back({{"name", f.value("name", "")},
                             {"description", f.value("description", "")},
                             {"parameters", f.value("parameters", json::object())}});
        }
        out.push_back({{"function_declarations", decls}});
    }
    return out;
}

// ---------------------------------------------------------------------------

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

static std::map<std::string, std::string> build_headers(Config& cfg) {
    std::map<std::string, std::string> headers;
    std::string st = cfg.style();
    if (st == "anthropic") {
        headers["x-api-key"] = cfg.api_key();
        headers["anthropic-version"] = "2023-06-01";
        headers["Content-Type"] = "application/json";
        headers["Accept"] = "text/event-stream";
    } else if (st == "gemini") {
        headers["x-goog-api-key"] = cfg.api_key();
        headers["Content-Type"] = "application/json";
    } else {
        headers["Authorization"] = "Bearer " + cfg.api_key();
        headers["Content-Type"] = "application/json";
        headers["Accept"] = "text/event-stream";
        if (cfg.provider == "openrouter") {
            headers["HTTP-Referer"] = "https://github.com/KOFA14888/gpt";
            headers["X-Title"] = "ScreenMate AI";
        }
    }
    return headers;
}

std::string agent_turn(Config& cfg, ToolContext& ctx, std::vector<json>& history,
                       const std::string& user_text, const AgentCallbacks& cb,
                       const std::string& mode_label) {
    if (cfg.api_key().empty())
        throw std::runtime_error("No API key. Open Settings (gear) or run: screenmate auth login");

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
             : "BUILD — полный доступ: правка файлов, shell, мышь и клавиатура "
               "(cursor: MOVE/CLICK/DRAG/SCROLL/KEY/TYPE).");

    std::string style = cfg.style();
    std::string sys = cfg.system_prompt + "\n\n" + mode_sys;
    json openai_tools = tool_schemas(ctx.mode);
    json wire_tools = convert_tools(style, openai_tools);
    std::string final_text;

    for (int step = 0; step < 40; ++step) {
        json payload;
        if (style == "anthropic") {
            payload = {{"model", cfg.model()},
                       {"max_tokens", 4096},
                       {"stream", true},
                       {"temperature", 0.3},
                       {"system", sys},
                       {"messages", convert_anthropic(history)}};
            if (!wire_tools.empty()) payload["tools"] = wire_tools;
        } else if (style == "gemini") {
            payload = {{"contents", convert_gemini(history)},
                       {"systemInstruction",
                        {{"parts", json::array({{{"text", sys}}})}}},
                       {"generationConfig",
                        {{"temperature", 0.3}, {"maxOutputTokens", 4096}}}};
            if (!wire_tools.empty()) payload["tools"] = wire_tools;
        } else {
            json messages = build_messages(cfg, history, mode_sys, true);
            payload = {
                {"model", cfg.model()},
                {"messages", messages},
                {"stream", true},
                {"temperature", 0.3},
                {"max_tokens", 4096},
            };
            if (!wire_tools.empty()) payload["tools"] = wire_tools;
        }

        std::map<std::string, std::string> headers = build_headers(cfg);

        StreamAccum acc;
        acc.style = style;
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
                "Model returned an empty response. Try another model (Settings / auth login).");

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
                                 {"name", name},
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
