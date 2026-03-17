#include <chrono>

#include <userver/engine/deadline.hpp>
#include <userver/utest/utest.hpp>

#include "deadline_utils.hpp"

using userver::engine::Deadline;
using namespace std::chrono_literals;

UTEST(DeadlineUtils, TimeLeftOrZeroExpiredIsZero)
{
    EXPECT_EQ(v1::timeLeftOrZeroMs(Deadline::Passed()), 0ms);
}

UTEST(DeadlineUtils, TimeLeftOrThrowExpiredThrows)
{
    EXPECT_THROW(
        static_cast<void>(v1::timeLeftOrThrowMs(Deadline::Passed(), "timeout")), std::runtime_error
    );
}

UTEST(DeadlineUtils, TimeLeftOrZeroUnreachableIsMax)
{
    const auto unreachable = Deadline{};
    EXPECT_FALSE(unreachable.IsReachable());
    EXPECT_EQ(v1::timeLeftOrZeroMs(unreachable), std::chrono::milliseconds::max());
}

UTEST(DeadlineUtils, SleepWithinDeadlineExpiredThrows)
{
    EXPECT_THROW(v1::sleepWithinDeadline(Deadline::Passed(), 1ms, "timeout"), std::runtime_error);
}

UTEST(DeadlineUtils, ChoosesExpiredOverFuture)
{
    const auto expired = Deadline::Passed();
    const auto future = Deadline::FromDuration(100ms);
    const auto chosen = v1::pickEarlierDeadline(future, expired);
    EXPECT_LE(chosen.TimeLeft(), 0ns);
}

UTEST(DeadlineUtils, ChoosesOtherExpiredOverFuture)
{
    const auto expired = Deadline::Passed();
    const auto future = Deadline::FromDuration(50ms);
    const auto chosen = v1::pickEarlierDeadline(expired, future);
    EXPECT_LE(chosen.TimeLeft(), 0ns);
}

UTEST(DeadlineUtils, BothExpiredStayExpired)
{
    const auto a = Deadline::Passed();
    const auto b = Deadline::Passed();
    const auto chosen = v1::pickEarlierDeadline(a, b);
    EXPECT_LE(chosen.TimeLeft(), 0ns);
}

UTEST(DeadlineUtils, PicksEarlierWhenBothReachable)
{
    const auto slow = Deadline::FromDuration(200ms);
    const auto fast = Deadline::FromDuration(50ms);
    const auto slowLeft = slow.TimeLeft();
    const auto fastLeft = fast.TimeLeft();
    const auto chosen = v1::pickEarlierDeadline(slow, fast);
    const auto expected = std::min(slowLeft, fastLeft);
    EXPECT_LE(chosen.TimeLeft(), expected + 2ms);
}
