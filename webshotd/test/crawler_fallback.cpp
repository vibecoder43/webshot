#include "crawler/fallback.hpp"

#include <userver/utest/utest.hpp>

#include <optional>
#include <vector>

using namespace v1::crawler;
using namespace text::literals;

namespace {

[[nodiscard]] AttemptSummary makeSuccessAttempt()
{
    return {
        .exited = true,
        .exitCode = 0,
        .waczExists = true,
        .seedProbe = {},
        .failureDetail = {},
    };
}

[[nodiscard]] AttemptSummary makeNoResponseFailureAttempt()
{
    return {
        .exited = true,
        .exitCode = 9,
        .waczExists = false,
        .seedProbe = SeedPageProbe{.status = 0, .loadState = 0},
        .failureDetail = {},
    };
}

} // namespace

UTEST(CrawlerFallback, HttpsSuccessDoesNotAttemptHttp)
{
    const auto httpsSeedUrl = "https://example.com/"_t;
    const auto httpSeedUrl = "http://example.com/"_t;

    std::vector<String> called;

    const auto result = runHttpsFirstWithHttpFallback(
        httpsSeedUrl, httpSeedUrl, [&](const String &seedUrl) {
            called.push_back(seedUrl);
            EXPECT_EQ(seedUrl, httpsSeedUrl);
            return makeSuccessAttempt();
        }
    );

    EXPECT_EQ(result.outcome, RunOutcome::kSucceeded);
    EXPECT_FALSE(result.httpAttempt.has_value());
    ASSERT_EQ(called.size(), 1);
    EXPECT_EQ(called[0], httpsSeedUrl);
}

UTEST(CrawlerFallback, HttpsNoResponseFallsBackToHttpSuccess)
{
    const auto httpsSeedUrl = "https://example.com/"_t;
    const auto httpSeedUrl = "http://example.com/"_t;

    std::vector<String> called;

    const auto result = runHttpsFirstWithHttpFallback(
        httpsSeedUrl, httpSeedUrl, [&](const String &seedUrl) {
            called.push_back(seedUrl);
            if (seedUrl == httpsSeedUrl)
                return makeNoResponseFailureAttempt();
            EXPECT_EQ(seedUrl, httpSeedUrl);
            return makeSuccessAttempt();
        }
    );

    EXPECT_EQ(result.outcome, RunOutcome::kSucceeded);
    ASSERT_TRUE(result.httpAttempt.has_value());
    ASSERT_EQ(called.size(), 2);
    EXPECT_EQ(called[0], httpsSeedUrl);
    EXPECT_EQ(called[1], httpSeedUrl);
}

UTEST(CrawlerFallback, HttpsNoResponseFallsBackToHttpFailure)
{
    const auto httpsSeedUrl = "https://example.com/"_t;
    const auto httpSeedUrl = "http://example.com/"_t;

    std::vector<String> called;

    const auto result = runHttpsFirstWithHttpFallback(
        httpsSeedUrl, httpSeedUrl, [&](const String &seedUrl) {
            called.push_back(seedUrl);
            if (seedUrl == httpsSeedUrl)
                return makeNoResponseFailureAttempt();
            EXPECT_EQ(seedUrl, httpSeedUrl);
            return makeNoResponseFailureAttempt();
        }
    );

    EXPECT_EQ(result.outcome, RunOutcome::kFailed);
    ASSERT_TRUE(result.httpAttempt.has_value());
    ASSERT_EQ(called.size(), 2);
    EXPECT_EQ(called[0], httpsSeedUrl);
    EXPECT_EQ(called[1], httpSeedUrl);
}
