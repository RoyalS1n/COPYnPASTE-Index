#pragma once
#include <cstdio>
#include <string>
#include <format>
#include <vector>

namespace df {
enum class LogLevel { Info, Warn, Error };
void logWrite(LogLevel level, const std::string& msg);

template <typename... A> void logInfo(std::format_string<A...> f, A&&... a) { logWrite(LogLevel::Info, std::format(f, std::forward<A>(a)...)); }
template <typename... A> void logWarn(std::format_string<A...> f, A&&... a) { logWrite(LogLevel::Warn, std::format(f, std::forward<A>(a)...)); }
template <typename... A> void logError(std::format_string<A...> f, A&&... a) { logWrite(LogLevel::Error, std::format(f, std::forward<A>(a)...)); }
[[noreturn]] void fatal(const std::string& msg);
std::vector<std::string> recentLog(size_t maxLines);
}  // namespace df
