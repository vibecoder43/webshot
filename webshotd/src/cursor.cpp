/**
 * @file
 * @brief Implementation of time and token helpers for pagination cursors.
 */

#include "cursor.hpp"

namespace ws::crud {

[[nodiscard]] int64_t TimePointToMicros(Clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
}

[[nodiscard]] Clock::time_point MicrosToTimePoint(int64_t micros)
{
    return Clock::time_point(std::chrono::microseconds(micros));
}

} // namespace ws::crud
