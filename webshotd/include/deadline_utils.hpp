#pragma once

#include "expected.hpp"
#include "invariant.hpp"
#include "try.hpp"

#include <algorithm>
#include <chrono>
#include <string_view>

#include <userver/engine/deadline.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/request/task_inherited_data.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;
namespace eng = us::engine;
using text::literals::operator""_t;

enum class DeadlineError {
    kTimeout,
};

[[nodiscard]] inline eng::Deadline PickEarlierDeadline(eng::Deadline a, eng::Deadline b)
{
    const bool a_reachable = a.IsReachable();
    const bool b_reachable = b.IsReachable();

    if (a_reachable && !b_reachable)
        return a;
    if (!a_reachable && b_reachable)
        return b;
    if (!a_reachable && !b_reachable)
        return a;

    if (a.TimeLeft() <= b.TimeLeft())
        return a;
    return b;
}

[[nodiscard]] inline eng::Deadline ComputeHandlerDeadline(
    const server::http::HttpRequest &request, std::chrono::milliseconds handler_timeout
)
{
    using eng::Deadline;

    const auto config_deadline = Deadline::FromTimePoint(request.GetStartTime() + handler_timeout);
    const auto inherited_deadline = server::request::GetTaskInheritedDeadline();

    return PickEarlierDeadline(config_deadline, inherited_deadline);
}

[[nodiscard]] inline std::chrono::milliseconds TimeLeftOrZeroMs(eng::Deadline deadline) noexcept
{
    using namespace std::chrono_literals;
    using Ms = std::chrono::milliseconds;

    if (!deadline.IsReachable())
        return Ms::max();

    const auto left = deadline.TimeLeft();
    if (left <= decltype(left)::zero())
        return 0ms;

    const auto left_ms = std::chrono::duration_cast<Ms>(left);
    if (left_ms <= 0ms)
        return 0ms;
    return left_ms;
}

[[nodiscard]] inline Expected<std::chrono::milliseconds, DeadlineError>
TimeLeftMs(eng::Deadline deadline) noexcept
{
    if (deadline.IsReachable() && deadline.IsReached())
        return Unex(DeadlineError::kTimeout);
    return TimeLeftOrZeroMs(deadline);
}

[[nodiscard]] inline Expected<void, DeadlineError>
SleepWithinDeadline(eng::Deadline deadline, std::chrono::milliseconds delay)
{
    using namespace std::chrono_literals;

    if (delay <= 0ms)
        return {};

    const auto sleep_for = std::min(delay, TRY(TimeLeftMs(deadline)));
    eng::SleepFor(sleep_for);
    if (sleep_for != delay)
        return Unex(DeadlineError::kTimeout);

    return {};
}

[[nodiscard]] inline Expected<void, DeadlineError> SleepUntilDeadline(eng::Deadline deadline)
{
    using namespace std::chrono_literals;

    Invariant(deadline.IsReachable(), "sleepUntilDeadline requires a reachable deadline"_t);

    const auto remaining = TRY(TimeLeftMs(deadline));
    ENSURE(remaining > 0ms, DeadlineError::kTimeout);

    eng::SleepFor(remaining);
    return {};
}

} // namespace ws
