#pragma once

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

#include <userver/engine/deadline.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/request/task_inherited_data.hpp>

namespace v1 {

[[nodiscard]] inline userver::engine::Deadline
pickEarlierDeadline(userver::engine::Deadline a, userver::engine::Deadline b)
{
    const bool aReachable = a.IsReachable();
    const bool bReachable = b.IsReachable();

    if (aReachable && !bReachable)
        return a;
    if (!aReachable && bReachable)
        return b;
    if (!aReachable && !bReachable)
        return a;

    if (a.TimeLeft() <= b.TimeLeft())
        return a;
    return b;
}

[[nodiscard]] inline userver::engine::Deadline computeHandlerDeadline(
    const userver::server::http::HttpRequest &request, std::chrono::milliseconds handlerTimeout
)
{
    using userver::engine::Deadline;

    const auto configDeadline = Deadline::FromTimePoint(request.GetStartTime() + handlerTimeout);
    const auto inheritedDeadline = userver::server::request::GetTaskInheritedDeadline();

    return pickEarlierDeadline(configDeadline, inheritedDeadline);
}

[[nodiscard]] inline std::chrono::milliseconds
timeLeftOrZeroMs(userver::engine::Deadline deadline) noexcept
{
    using namespace std::chrono_literals;
    using Ms = std::chrono::milliseconds;

    if (!deadline.IsReachable())
        return Ms::max();

    const auto left = deadline.TimeLeft();
    if (left <= decltype(left)::zero())
        return 0ms;

    const auto leftMs = std::chrono::duration_cast<Ms>(left);
    if (leftMs <= 0ms)
        return 0ms;
    return leftMs;
}

[[nodiscard]] inline std::chrono::milliseconds
timeLeftOrThrowMs(userver::engine::Deadline deadline, std::string_view timeoutMessage)
{
    if (deadline.IsReachable() && deadline.IsReached())
        throw std::runtime_error(std::string(timeoutMessage));
    return timeLeftOrZeroMs(deadline);
}

inline void sleepWithinDeadline(
    userver::engine::Deadline deadline, std::chrono::milliseconds delay,
    std::string_view timeoutMessage
)
{
    using namespace std::chrono_literals;

    if (delay <= 0ms)
        return;

    const auto remaining = timeLeftOrThrowMs(deadline, timeoutMessage);
    const auto sleepFor = std::min(delay, remaining);
    userver::engine::SleepFor(sleepFor);
    if (sleepFor != delay)
        throw std::runtime_error(std::string(timeoutMessage));
}

} // namespace v1
