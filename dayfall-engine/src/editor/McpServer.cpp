#include "editor/McpServer.h"
#include "core/Log.h"
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace df {
using json = nlohmann::json;
namespace {
constexpr const char* kProtocols[] = {"2025-06-18", "2025-03-26", "2024-11-05"};
json rpcError(const json& id, int code, const std::string& msg) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}
json rpcResult(const json& id, json result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }
}  // namespace

json McpServer::toolList() const {
    json tools = json::array();
    for (const Tool& t : editor_.tools()) {
        bool readOnly = t.category == ToolCategory::Read;
        json annotations = {{"readOnlyHint", readOnly}, {"destructiveHint", t.name == "delete" || t.name == "terrain_generate"},
                            {"idempotentHint", readOnly}, {"openWorldHint", false}};
        json schema = t.schema;
        if (!schema.contains("properties")) schema["properties"] = json::object();
        tools.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", schema}, {"annotations", annotations}});
    }
    return tools;
}

std::optional<json> McpServer::handleText(const std::string& text) {
    json msg;
    try {
        msg = json::parse(text);
    } catch (const std::exception& e) {
        return rpcError(nullptr, -32700, std::string("parse error: ") + e.what());
    }
    return handle(msg);
}

std::optional<json> McpServer::handle(const json& msg) {
    if (msg.is_array()) {   // JSON-RPC batch (older protocol revisions)
        json out = json::array();
        for (auto& m : msg) if (auto r = handle(m)) out.push_back(*r);
        if (out.empty()) return std::nullopt;
        return out;
    }
    if (!msg.is_object() || !msg.contains("method")) {
        if (msg.is_object() && (msg.contains("result") || msg.contains("error"))) return std::nullopt;   // a response to us: ignore
        return rpcError(msg.is_object() ? msg.value("id", json()) : json(), -32600, "invalid request");
    }
    std::string method = msg["method"].get<std::string>();
    bool notification = !msg.contains("id");
    json id = msg.value("id", json());
    const json params = msg.value("params", json::object());
    if (notification) return std::nullopt;   // notifications/initialized, notifications/cancelled, ...

    if (method == "initialize") {
        std::string want = params.value("protocolVersion", kProtocols[0]);
        std::string use = kProtocols[0];
        for (const char* p : kProtocols) if (want == p) use = p;
        logInfo("MCP client connected: {} {} (protocol {})", params.value("clientInfo", json::object()).value("name", "?"),
                params.value("clientInfo", json::object()).value("version", ""), use);
        return rpcResult(id, {{"protocolVersion", use},
                              {"capabilities", {{"tools", {{"listChanged", false}}}}},
                              {"serverInfo", {{"name", "dayfall"}, {"title", "DAYFALL engine"}, {"version", "0.1.0"}}},
                              {"instructions", editor_.instructions()}});
    }
    if (method == "ping") return rpcResult(id, json::object());
    if (method == "tools/list") return rpcResult(id, {{"tools", toolList()}});
    if (method == "tools/call") {
        std::string name = params.value("name", "");
        json args = params.value("arguments", json::object());
        ToolResult res;
        ++calls;
        auto t0 = std::chrono::steady_clock::now();
        queue_.post([&] { res = editor_.call(name, args); }).wait();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        logInfo("tool {} {} in {:.0f} ms", name, res.error ? "FAILED" : "ok", ms);
        json content = json::array();
        content.push_back({{"type", "text"}, {"text", res.data.dump(1)}});
        for (auto& img : res.images) {
            content.push_back({{"type", "image"}, {"data", img.base64}, {"mimeType", img.mime}});
        }
        return rpcResult(id, {{"content", content}, {"isError", res.error}});
    }
    if (method == "resources/list") return rpcResult(id, {{"resources", json::array()}});
    if (method == "prompts/list") return rpcResult(id, {{"prompts", json::array()}});
    return rpcError(id, -32601, "method not found: " + method);
}

// ------------------------------------------------------------------------------------- stdio
void StdioTransport::start(McpServer& server, std::atomic<bool>& quit) {
    thread_ = std::thread([&server, &quit] {
        std::mutex outMutex;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty() || line == "\r") continue;
            auto resp = server.handleText(line);
            if (resp) {
                std::lock_guard lock(outMutex);
                std::string s = resp->dump();
                std::fwrite(s.data(), 1, s.size(), stdout);
                std::fputc('\n', stdout);
                std::fflush(stdout);
            }
        }
        logInfo("MCP stdin closed: shutting down");
        quit = true;
    });
}

void StdioTransport::stop() {
    if (thread_.joinable()) thread_.detach();   // blocked in getline; the process is exiting
}

// ------------------------------------------------------------------------------------- http
namespace {
void closeSock(intptr_t s) {
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    ::close((int)s);
#endif
}
bool sendAll(intptr_t s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        auto n = ::send((decltype(socket(0, 0, 0)))s, data.data() + off, (int)(data.size() - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
}  // namespace

bool HttpTransport::start(McpServer& server, const std::string& host, int port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    server_ = &server;
    auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 8) != 0) {
        closeSock((intptr_t)s);
        logError("MCP http: cannot listen on {}:{} (in use?)", host, port);
        return false;
    }
    listen_ = (intptr_t)s;
    port_ = port;
    std::random_device rd;
    sessionId_ = std::format("{:08x}{:08x}", rd(), rd());
    running_ = true;
    thread_ = std::thread([this] {
        while (running_) {
            sockaddr_in ca{};
            socklen_t len = sizeof(ca);
            auto c = accept((decltype(socket(0, 0, 0)))listen_, (sockaddr*)&ca, &len);
            if (!running_) break;
            if ((intptr_t)c < 0) continue;
            std::thread([this, c] { serve((intptr_t)c); }).detach();
        }
    });
    logInfo("MCP server listening on http://{}:{}/mcp", host, port);
    return true;
}

void HttpTransport::serve(intptr_t c) {
    std::string buf;
    char tmp[65536];
    for (;;) {
        // headers
        size_t hdrEnd;
        while ((hdrEnd = buf.find("\r\n\r\n")) == std::string::npos) {
            auto n = recv((decltype(socket(0, 0, 0)))c, tmp, sizeof(tmp), 0);
            if (n <= 0) { closeSock(c); return; }
            buf.append(tmp, (size_t)n);
            if (buf.size() > (1u << 20)) { closeSock(c); return; }
        }
        std::istringstream hs(buf.substr(0, hdrEnd));
        std::string method, path, version, line;
        hs >> method >> path >> version;
        std::getline(hs, line);
        std::map<std::string, std::string> headers;
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string v = line.substr(colon + 1);
            v.erase(0, v.find_first_not_of(' '));
            headers[lower(line.substr(0, colon))] = v;
        }
        size_t length = headers.count("content-length") ? std::stoul(headers["content-length"]) : 0;
        while (buf.size() < hdrEnd + 4 + length) {
            auto n = recv((decltype(socket(0, 0, 0)))c, tmp, sizeof(tmp), 0);
            if (n <= 0) { closeSock(c); return; }
            buf.append(tmp, (size_t)n);
        }
        std::string body = buf.substr(hdrEnd + 4, length);
        buf.erase(0, hdrEnd + 4 + length);
        auto reply = [&](int code, const std::string& status, const std::string& type, const std::string& content, const std::string& extra = "") {
            std::string r = std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: keep-alive\r\n{}\r\n", code,
                                        status, type, content.size(), extra);
            return sendAll(c, r + content);
        };
        // DNS-rebinding protection: only local origins
        if (headers.count("origin")) {
            std::string o = lower(headers["origin"]);
            if (o.find("://localhost") == std::string::npos && o.find("://127.0.0.1") == std::string::npos) {
                reply(403, "Forbidden", "text/plain", "origin not allowed");
                continue;
            }
        }
        bool ok = true;
        if (path.rfind("/mcp", 0) == 0) {
            if (method == "POST") {
                auto resp = server_->handleText(body);
                std::string extra = std::format("Mcp-Session-Id: {}\r\n", sessionId_);
                ok = resp ? reply(200, "OK", "application/json", resp->dump(), extra) : reply(202, "Accepted", "text/plain", "", extra);
            } else if (method == "DELETE") {
                ok = reply(200, "OK", "text/plain", "");
            } else {
                ok = reply(405, "Method Not Allowed", "text/plain", "this server does not open SSE streams; POST JSON-RPC to /mcp", "Allow: POST, DELETE\r\n");
            }
        } else if (method == "GET" && path == "/") {
            ok = reply(200, "OK", "text/plain", std::format("DAYFALL engine MCP server\nPOST JSON-RPC to /mcp\ntool calls served: {}\n", server_->calls.load()));
        } else {
            ok = reply(404, "Not Found", "text/plain", "not found");
        }
        if (!ok) { closeSock(c); return; }
    }
}

void HttpTransport::stop() {
    if (!running_) return;
    running_ = false;
#ifndef _WIN32
    ::shutdown((int)listen_, SHUT_RDWR);
#endif
    closeSock(listen_);
    if (thread_.joinable()) thread_.join();
}
}  // namespace df
