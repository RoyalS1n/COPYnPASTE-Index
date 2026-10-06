#pragma once
#include <stdexcept>
#include <string>

namespace df {
// Recoverable error (bad input, missing file, invalid edit). The editor and the
// MCP server report it to the caller and keep running; fatal() is only for
// GPU / driver failures.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
}  // namespace df
