#include <chrono>

#include <userver/engine/deadline.hpp>
#include <userver/utest/utest.hpp>

#include "deadline_utils.hpp"

namespace ws {
namespace us = userver;
namespace eng = us::engine;
} // namespace ws

using namespace ws;

using eng::Deadline;
using namespace std::chrono_literals;

UTEST(DeadlineUtils, TimeLeftOrZeroExpiredIsZero)
{
    EXPECT_EQ(ws::TimeLeftOrZeroMs(Deadline::Passed()), 0ms);
}

UTEST(DeadlineUtils, TimeLeftOrThrowExpiredThrows)
{
    auto left = ws::TimeLeftMs(Deadline::Passed());
    ASSERT_FALSE(left);
    EXPECT_EQ(left.Error(), ws::DeadlineError::kTimeout);
}

UTEST(DeadlineUtils, TimeLeftOrZeroUnreachableIsMax)
{
    const Deadline unreachable{};
    EXPECT_FALSE(unreachable.IsReachable());
    EXPECT_EQ(ws::TimeLeftOrZeroMs(unreachable), std::chrono::milliseconds::max());
}

UTEST(DeadlineUtils, SleepWithinDeadlineExpiredThrows)
{
    auto slept = ws::SleepWithinDeadline(Deadline::Passed(), 1ms);
    ASSERT_FALSE(slept);
    EXPECT_EQ(slept.Error(), ws::DeadlineError::kTimeout);
}

UTEST(DeadlineUtils, SleepUntilDeadlineExpiredThrows)
{
    auto slept = ws::SleepUntilDeadline(Deadline::Passed());
    ASSERT_FALSE(slept);
    EXPECT_EQ(slept.Error(), ws::DeadlineError::kTimeout);
}

UTEST(DeadlineUtils, ChoosesExpiredOverFuture)
{
    auto expired = Deadline::Passed();
    auto future = Deadline::FromDuration(100ms);
    auto chosen = ws::PickEarlierDeadline(future, expired);
    EXPECT_LE(chosen.TimeLeft(), 0ns);
}

UTEST(DeadlineUtils, ChoosesOtherExpiredOverFuture)
{
    auto expired = Deadline::Passed();
    auto future = Deadline::FromDuration(50ms);
    auto chosen = ws::PickEarlierDeadline(expired, future);
    EXPECT_LE(chosen.TimeLeft(), 0ns);
}

UTEST(DeadlineUtils, BothExpiredStayExpired)
{
    auto a = Deadline::Passed();
    auto b = Deadline::Passed();
    auto chosen = ws::PickEarlierDeadline(a, b);
    EXPECT_LE(chosen.TimeLeft(), 0ns);
}

UTEST(DeadlineUtils, PicksEarlierWhenBothReachable)
{
    auto slow = Deadline::FromDuration(200ms);
    auto fast = Deadline::FromDuration(50ms);
    auto slow_left = slow.TimeLeft();
    auto fast_left = fast.TimeLeft();
    auto chosen = ws::PickEarlierDeadline(slow, fast);
    auto expected = std::min(slow_left, fast_left);
    EXPECT_LE(chosen.TimeLeft(), expected + 2ms);
}
