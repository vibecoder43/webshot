#pragma once

#include "integers.hpp"

#include <chrono>

namespace v1::s3refresh {

/**
 * @brief Compute delay before refreshing STS credentials.
 *
 * Returns max(expiresAt - now - margin, 0s) in whole seconds.
 */
[[nodiscard]] inline std::chrono::seconds computeRefreshDelay(
    std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point expiresAt,
    i64 marginSec
)
{
    auto delay = expiresAt - now - toSeconds(marginSec);
    if (delay < std::chrono::seconds(0))
        return std::chrono::seconds(0);
    return std::chrono::duration_cast<std::chrono::seconds>(delay);
}

} // namespace v1::s3refresh
