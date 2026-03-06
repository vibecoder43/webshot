#include <chrono>

#include <userver/engine/deadline.hpp>
#include <userver/utest/utest.hpp>

#include "deadline_utils.hpp"

using userver::engine::Deadline;
using namespace std::chrono_literals;

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
