#include <chrono>

#include <userver/utest/utest.hpp>
#include <userver/utils/datetime.hpp>

#include "s3_refresh_utils.hpp"
#include "userver_namespaces.hpp"

using std::chrono::system_clock;
using v1::s3refresh::computeRefreshDelay;
using namespace std::chrono_literals;

UTEST(S3RefreshSchedule, FutureExpirationRespectsMargin)
{
    const auto now = datetime::Now();
    const auto expiresAt = now + 600s;
    const auto delay = computeRefreshDelay(now, expiresAt, 120s);
    EXPECT_EQ(delay, 480s);
}

UTEST(S3RefreshSchedule, PastOrNearExpirationClampsToZero)
{
    const auto now = datetime::Now();
    const auto expiresAt = now + 30s;
    const auto delay = computeRefreshDelay(now, expiresAt, 60s);
    EXPECT_EQ(delay, 0s);
}
