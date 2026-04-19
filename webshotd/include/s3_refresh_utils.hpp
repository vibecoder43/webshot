#pragma once

#include <chrono>

namespace v1::s3refresh {

/**
 * @brief Compute delay before refreshing STS credentials.
 *
 * Returns max(expiresAt - now - margin, 0s) in whole seconds.
 */
[[nodiscard]] inline std::chrono::seconds computeRefreshDelay(
    std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point expiresAt,
    std::chrono::seconds margin
)
{
    using namespace std::chrono_literals;

    auto delay = expiresAt - now - margin;
    if (delay < 0s)
        return 0s;
    return std::chrono::duration_cast<std::chrono::seconds>(delay);
}

} // namespace v1::s3refresh
