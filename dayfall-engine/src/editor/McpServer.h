#pragma once
// Model Context Protocol server: JSON-RPC 2.0 over stdio (Claude Code launches
// the engine from .mcp.json) or Streamable HTTP on localhost (attach to a running
// editor). Serves the tools, read-only resources (the docs, the live map document,
// the captures) and workflow prompts. Tool calls and world reads run on the main
// thread through MainThreadQueue; long tools report progress and can be cancelled.
#include "editor/Editor.h"
#include "editor/MainThread.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>

namespace df {
// One client connection: the stdio stream, or one HTTP session (Mcp-Session-Id).
class McpSession {
public:
    McpSession(std::string id, bool stdio, std::string protocol) : id(std::move(id)), stdio(stdio), protocol_(std::move(protocol)) {}
    const std::string id;
    const bool stdio;
    std::string protocol() const { std::lock_guard lock(m_); return protocol_; }
    void setProtocol(const std::string& p) { std::lock_guard lock(m_); protocol_ = p; }
    std::atomic<bool> initialized{false};
    std::atomic<int> logLevel{3};        // notifications/message from this level up (3: warning)
private:
    mutable std::mutex m_;
    std::string protocol_;
};

// Sends a notification (progress) on the channel of the request being handled; called from the main thread.
using McpNotify = std::function<void(const nlohmann::json&)>;

class McpServer {
public:
    McpServer(Editor& editor, MainThreadQueue& queue) : editor_(editor), queue_(queue) {}
    // Handles one message or batch; returns the response (nullopt for notifications, and for requests the
    // client cancelled over stdio). notify carries the progress notifications of a tools/call. Thread-safe.
    std::optional<nlohmann::json> handle(const nlohmann::json& msg, McpSession& session, const McpNotify& notify = {});
    std::optional<nlohmann::json> handleText(const std::string& text, McpSession& session, const McpNotify& notify = {});
    static bool supportsProtocol(const std::string& version);
    static const char* latestProtocol();
    static const char* defaultProtocol() { return "2025-03-26"; }   // a client that never said (Streamable HTTP rule)
    std::atomic<int> calls{0};
private:
    struct Call { std::atomic<bool> cancelled{false}; };
    nlohmann::json handleRequest(const std::string& method, const nlohmann::json& id, const nlohmann::json& params, McpSession& s,
                                 const McpNotify& notify, bool& noResponse);
    nlohmann::json toolList() const;
    nlohmann::json callTool(const nlohmann::json& id, const nlohmann::json& params, McpSession& s, const McpNotify& notify, bool& noResponse);
    nlohmann::json resourceList();
    nlohmann::json resourceTemplates() const;
    nlohmann::json readResource(const std::string& uri);
    nlohmann::json promptList() const;
    nlohmann::json getPrompt(const std::string& name, const nlohmann::json& args) const;
    void cancel(const McpSession& s, const nlohmann::json& requestId);
    std::filesystem::path mapDir();      // the open map's folder (read on the main thread)
    Editor& editor_;
    MainThreadQueue& queue_;
    std::mutex callsMutex_;
    std::map<std::string, std::shared_ptr<Call>> inFlight_;   // "<session>|<request id>" -> tools/call
    std::deque<std::string> cancelledEarly_;                   // cancellations that arrived before their request
    std::shared_ptr<Call> running_;                            // the call on the main thread now
};

class StdioTransport {
public:
    void start(McpServer& server, std::atomic<bool>& quit);
    void stop();
private:
    void write(const nlohmann::json& msg);   // one line on stdout; any thread
    std::thread thread_;
    McpSession session_{"stdio", true, McpServer::defaultProtocol()};
    std::mutex out_;
    std::mutex pendingMutex_;
    std::condition_variable pendingCv_;
    int pending_ = 0;                        // requests being handled on worker threads
};

class HttpTransport {
public:
    bool start(McpServer& server, const std::string& host, int port);   // false if the port is taken
    void stop();
    int port() const { return port_; }
    static constexpr size_t kMaxBody = 16u << 20;
private:
    void serve(intptr_t client);
    std::shared_ptr<McpSession> session(const std::string& id);
    std::shared_ptr<McpSession> newSession(const std::string& protocol);
    McpServer* server_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    intptr_t listen_ = -1;
    int port_ = 0;
    std::mutex sessionsMutex_;
    std::map<std::string, std::shared_ptr<McpSession>> sessions_;
    std::deque<std::string> sessionOrder_;   // oldest first; the oldest are dropped past 256 sessions
};
}  // namespace df
