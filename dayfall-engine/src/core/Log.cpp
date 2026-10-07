#include "core/Log.h"
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>

namespace df {
static std::mutex g_logMutex;
static const auto g_start = std::chrono::steady_clock::now();
static std::deque<std::string> g_recent;
static uint64_t g_warnings = 0, g_errors = 0;
static std::function<void(LogLevel, const std::string&)> g_sink;

static void writeLine(LogLevel level, const std::string& msg) {
    std::lock_guard lock(g_logMutex);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    const char* tag = level == LogLevel::Info ? "info" : level == LogLevel::Warn ? "warn" : "ERROR";
    // everything goes to stderr: stdout carries the MCP protocol in --mcp stdio mode
    std::fprintf(stderr, "[%8.3f %s] %s\n", t, tag, msg.c_str());
    std::fflush(stderr);
    g_recent.push_back(std::format("[{:8.3f} {}] {}", t, tag, msg));
    if (g_recent.size() > 500) g_recent.pop_front();
    if (level == LogLevel::Warn) ++g_warnings;
    if (level == LogLevel::Error) ++g_errors;
}

void logWrite(LogLevel level, const std::string& msg) {
    writeLine(level, msg);
    std::function<void(LogLevel, const std::string&)> sink;
    {
        std::lock_guard lock(g_logMutex);
        sink = g_sink;
    }
    if (sink) sink(level, msg);   // outside the lock: the sink may block on its own output
}

void setLogSink(std::function<void(LogLevel, const std::string&)> sink) {
    std::lock_guard lock(g_logMutex);
    g_sink = std::move(sink);
}

std::vector<std::string> recentLog(size_t maxLines) {
    std::lock_guard lock(g_logMutex);
    size_t n = std::min(maxLines, g_recent.size());
    return std::vector<std::string>(g_recent.end() - (std::ptrdiff_t)n, g_recent.end());
}

void fatal(const std::string& msg) {
    logWrite(LogLevel::Error, msg);
    std::exit(1);
}
}  // namespace df
