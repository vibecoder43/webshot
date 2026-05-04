#pragma once

#include <chrono>

namespace ws::s3refresh {

/**
 * @brief Compute delay before refreshing STS credentials.
 *
 * Returns max(expiresAt - now - margin, 0s) in whole seconds.
 */
[[nodiscard]] inline std::chrono::seconds ComputeRefreshDelay(
    std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point expires_at,
    std::chrono::seconds margin
)
{
    using namespace std::chrono_literals;

    auto delay = expires_at - now - margin;
    if (delay < 0s)
        return 0s;
    return std::chrono::duration_cast<std::chrono::seconds>(delay);
}

} // namespace ws::s3refresh
