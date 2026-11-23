#pragma once
// Agent loop: streams chat completions with OpenAI-style tool calls.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "tools.hpp"

namespace sm {

struct AgentCallbacks {
    // streamed assistant text delta
    std::function<void(const std::string&)> on_text;
    // "→ tool_name(args)" etc.
    std::function<void(const std::string&)> on_status;
};

// One conversation turn (may loop over tool calls). Returns final assistant text.
// Throws std::runtime_error on API failure.
std::string agent_turn(Config& cfg, ToolContext& ctx,
                       std::vector<nlohmann::json>& history,
                       const std::string& user_text,
                       const AgentCallbacks& cb,
                       const std::string& mode_label);

// Build request messages from history + system prompt.
nlohmann::json build_messages(Config& cfg, const std::vector<nlohmann::json>& history,
                              const std::string& extra_system);

// Parse SSE `data:` payloads from an HTTP stream body already split by caller.
// Used by http layer via callback — here we accumulate tool_calls.
struct StreamAccum {
    // wire protocol: "openai" | "anthropic" | "gemini" (default openai)
    std::string style = "openai";
    std::string text;
    // index -> {id, name, args}
    std::map<int, nlohmann::json> tool_calls;
    std::string finish_reason;
    void feed(const std::string& data_line);
    bool has_tools() const { return !tool_calls.empty(); }
};

}  // namespace sm
