#pragma once

#include "expected.hpp"

#include <algorithm>
#include <chrono>
#include <string_view>

#include <userver/engine/deadline.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/request/task_inherited_data.hpp>

namespace v1 {

enum class DeadlineError {
    kTimeout,
};

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

[[nodiscard]] inline Expected<std::chrono::milliseconds, DeadlineError>
timeLeftMs(userver::engine::Deadline deadline) noexcept
{
    if (deadline.IsReachable() && deadline.IsReached())
        return std::unexpected(DeadlineError::kTimeout);
    return timeLeftOrZeroMs(deadline);
}

[[nodiscard]] inline Expected<void, DeadlineError>
sleepWithinDeadline(userver::engine::Deadline deadline, std::chrono::milliseconds delay)
{
    using namespace std::chrono_literals;

    if (delay <= 0ms)
        return {};

    const auto remaining = timeLeftMs(deadline);
    if (!remaining)
        return std::unexpected(remaining.error());

    const auto sleepFor = std::min(delay, remaining.value());
    userver::engine::SleepFor(sleepFor);
    if (sleepFor != delay)
        return std::unexpected(DeadlineError::kTimeout);

    return {};
}

[[nodiscard]] inline Expected<void, DeadlineError>
sleepUntilDeadline(userver::engine::Deadline deadline)
{
    using namespace std::chrono_literals;

    UINVARIANT(deadline.IsReachable(), "sleepUntilDeadline requires a reachable deadline");

    const auto remaining = timeLeftMs(deadline);
    if (!remaining)
        return std::unexpected(remaining.error());
    if (remaining.value() <= 0ms)
        return std::unexpected(DeadlineError::kTimeout);

    userver::engine::SleepFor(remaining.value());
    return {};
}

} // namespace v1
