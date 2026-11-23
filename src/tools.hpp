#pragma once
// Tool registry for the coding agent: files, shell, search, screen, cursor, LSP.

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"

namespace sm {

enum class AgentMode { Build, Plan };

struct ToolContext {
    Config* cfg = nullptr;
    AgentMode mode = AgentMode::Build;
    fs::path cwd;  // project root
    // optional: inject latest screenshot bytes when tool "screenshot" runs
    std::string last_screenshot_jpeg;
};

// OpenAI-style tool schemas for the current mode.
nlohmann::json tool_schemas(AgentMode mode);

// Execute a tool call. Returns {"ok":bool,"content":string,"image_b64":optional}
nlohmann::json run_tool(ToolContext& ctx, const std::string& name,
                        const nlohmann::json& args);

// Extract cursor commands from free text (legacy [MOVE x y] protocol) and run them.
std::vector<std::string> run_bracket_commands(const std::string& text, ToolContext& ctx);

}  // namespace sm
