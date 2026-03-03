#include <chrono>

#include <userver/utest/utest.hpp>
#include <userver/utils/datetime.hpp>

#include "s3_refresh_utils.hpp"

using std::chrono::seconds;
using std::chrono::system_clock;
using v1::s3refresh::computeRefreshDelay;
namespace datetime = userver::utils::datetime;

UTEST(S3RefreshSchedule, FutureExpirationRespectsMargin)
{
    const auto now = datetime::Now();
    const auto expiresAt = now + seconds(600);
    const auto delay = computeRefreshDelay(now, expiresAt, 120);
    EXPECT_EQ(delay, seconds(480));
}

UTEST(S3RefreshSchedule, PastOrNearExpirationClampsToZero)
{
    const auto now = datetime::Now();
    const auto expiresAt = now + seconds(30);
    const auto delay = computeRefreshDelay(now, expiresAt, 60);
    EXPECT_EQ(delay, seconds(0));
}
