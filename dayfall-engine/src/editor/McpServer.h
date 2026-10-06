#pragma once
// Model Context Protocol server: JSON-RPC 2.0 over stdio (Claude Code launches
// the engine from .mcp.json) or HTTP POST on localhost (attach to a running
// editor). Tool calls run on the main thread through MainThreadQueue.
#include "editor/Editor.h"
#include "editor/MainThread.h"
#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>

namespace df {
class McpServer {
public:
    McpServer(Editor& editor, MainThreadQueue& queue) : editor_(editor), queue_(queue) {}
    // Handles one message; returns the response (nullopt for notifications). Thread-safe.
    std::optional<nlohmann::json> handle(const nlohmann::json& msg);
    std::optional<nlohmann::json> handleText(const std::string& text);   // parse errors become JSON-RPC errors
    std::atomic<int> calls{0};
private:
    nlohmann::json toolList() const;
    Editor& editor_;
    MainThreadQueue& queue_;
};

class StdioTransport {
public:
    void start(McpServer& server, std::atomic<bool>& quit);
    void stop();
private:
    std::thread thread_;
};

class HttpTransport {
public:
    bool start(McpServer& server, const std::string& host, int port);   // false if the port is taken
    void stop();
    int port() const { return port_; }
private:
    void serve(intptr_t client);
    McpServer* server_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    intptr_t listen_ = -1;
    int port_ = 0;
    std::string sessionId_;
};
}  // namespace df
