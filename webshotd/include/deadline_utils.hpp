#pragma once

#include "expected.hpp"
#include "try.hpp"
#include "userver_namespaces.hpp"

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

[[nodiscard]] inline eng::Deadline pickEarlierDeadline(eng::Deadline a, eng::Deadline b)
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

[[nodiscard]] inline eng::Deadline computeHandlerDeadline(
    const server::http::HttpRequest &request, std::chrono::milliseconds handlerTimeout
)
{
    using eng::Deadline;

    const auto configDeadline = Deadline::FromTimePoint(request.GetStartTime() + handlerTimeout);
    const auto inheritedDeadline = server::request::GetTaskInheritedDeadline();

    return pickEarlierDeadline(configDeadline, inheritedDeadline);
}

[[nodiscard]] inline std::chrono::milliseconds timeLeftOrZeroMs(eng::Deadline deadline) noexcept
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
timeLeftMs(eng::Deadline deadline) noexcept
{
    if (deadline.IsReachable() && deadline.IsReached())
        return Unex(DeadlineError::kTimeout);
    return timeLeftOrZeroMs(deadline);
}

[[nodiscard]] inline Expected<void, DeadlineError>
sleepWithinDeadline(eng::Deadline deadline, std::chrono::milliseconds delay)
{
    using namespace std::chrono_literals;

    if (delay <= 0ms)
        return {};

    const auto sleepFor = std::min(delay, TRY(timeLeftMs(deadline)));
    eng::SleepFor(sleepFor);
    if (sleepFor != delay)
        return Unex(DeadlineError::kTimeout);

    return {};
}

[[nodiscard]] inline Expected<void, DeadlineError> sleepUntilDeadline(eng::Deadline deadline)
{
    using namespace std::chrono_literals;

    invariant(deadline.IsReachable(), "sleepUntilDeadline requires a reachable deadline");

    const auto remaining = TRY(timeLeftMs(deadline));
    ENSURE(remaining > 0ms, DeadlineError::kTimeout);

    eng::SleepFor(remaining);
    return {};
}

} // namespace v1
