#pragma once
#include <volk.h>
#include "core/Log.h"

#define VK_CHECK(expr)                                                                        \
    do {                                                                                      \
        VkResult r_ = (expr);                                                                 \
        if (r_ != VK_SUCCESS) ::df::fatal(std::format("{} failed: VkResult {}", #expr, (int)r_)); \
    } while (0)
