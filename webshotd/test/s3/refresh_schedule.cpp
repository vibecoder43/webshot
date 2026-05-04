#include <chrono>

#include <userver/utest/utest.hpp>
#include <userver/utils/datetime.hpp>

#include "s3/refresh_utils.hpp"

namespace ws {
namespace us = userver;
namespace datetime = us::utils::datetime;
} // namespace ws

using namespace ws;

using std::chrono::system_clock;
using ws::s3refresh::ComputeRefreshDelay;
using namespace std::chrono_literals;

UTEST(RefreshSchedule, FutureExpirationRespectsMargin)
{
    const auto now = datetime::Now();
    const auto expires_at = now + 600s;
    const auto delay = ComputeRefreshDelay(now, expires_at, 120s);
    EXPECT_EQ(delay, 480s);
}

UTEST(RefreshSchedule, PastOrNearExpirationClampsToZero)
{
    const auto now = datetime::Now();
    const auto expires_at = now + 30s;
    const auto delay = ComputeRefreshDelay(now, expires_at, 60s);
    EXPECT_EQ(delay, 0s);
}
