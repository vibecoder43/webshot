/**
 * @file
 * @brief Implementation of time and token helpers for pagination cursors.
 */

#include "webshot_cursor.hpp"

namespace v1::crud {

[[nodiscard]] int64_t timePointToMicros(Clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
}

[[nodiscard]] Clock::time_point microsToTimePoint(int64_t micros)
{
    return Clock::time_point(std::chrono::microseconds(micros));
}

} // namespace v1::crud
