#pragma once
// Tool registry for the coding agent: files, shell, search, screen, LSP.

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
};

// OpenAI-style tool schemas for the current mode.
nlohmann::json tool_schemas(AgentMode mode);

// Execute a tool call. Returns {"ok":bool,"content":string}
nlohmann::json run_tool(ToolContext& ctx, const std::string& name,
                        const nlohmann::json& args);

}  // namespace sm
