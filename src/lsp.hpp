#pragma once
// Minimal LSP client: initialize, definition, hover, diagnostics.
// Spawns a configured language server for a file extension.

#include <map>
#include <memory>
#include <string>

namespace sm {

class LspServer {
public:
    ~LspServer();
    LspServer() = default;
    LspServer(const LspServer&) = delete;
    LspServer& operator=(const LspServer&) = delete;

    bool start(const std::string& command, std::string* error);
    bool running() const;

    // Sync-ish helpers with short timeouts.
    std::string initialize(const std::string& root_uri);
    std::string definition(const std::string& path, const std::string& text,
                           int line, int character);
    std::string hover(const std::string& path, const std::string& text,
                      int line, int character);

    void did_open(const std::string& path, const std::string& text);
    void shutdown();

public:
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Process-lifetime singleton so callers never destroy Impl in other TUs.
LspServer& lsp_global_server();


// Pick server command from config map by extension (".py" -> ...).
std::string lsp_command_for(const std::map<std::string, std::string>& servers,
                            const std::string& path);

}  // namespace sm
