#include "crawler/fallback.hpp"

#include <userver/utest/utest.hpp>

#include <optional>
#include <vector>

using namespace ws::crawler;
using namespace text::literals;

namespace {

[[nodiscard]] AttemptSummary MakeSuccessAttempt()
{
    return {
        .exited = true,
        .exit_code = 0,
        .wacz_exists = true,
        .seed_probe = {},
        .failure_detail = {},
    };
}

[[nodiscard]] AttemptSummary MakeNoResponseFailureAttempt()
{
    return {
        .exited = true,
        .exit_code = 9,
        .wacz_exists = false,
        .seed_probe = SeedPageProbe{.status = 0, .load_state = 0},
        .failure_detail = {},
    };
}

} // namespace

UTEST(CrawlerFallback, HttpsSuccessDoesNotAttemptHttp)
{
    const auto https_seed_url = "https://example.com/"_t;
    const auto http_seed_url = "http://example.com/"_t;

    std::vector<String> called;

    const auto result = RunHttpsFirstWithHttpFallback(
        https_seed_url, http_seed_url, [&](const String &seed_url) {
            called.push_back(seed_url);
            EXPECT_EQ(seed_url, https_seed_url);
            return MakeSuccessAttempt();
        }
    );

    EXPECT_EQ(result.outcome, RunOutcome::kSucceeded);
    EXPECT_FALSE(result.http_attempt);
    ASSERT_EQ(called.size(), 1);
    EXPECT_EQ(called[0], https_seed_url);
}

UTEST(CrawlerFallback, HttpsNoResponseFallsBackToHttpSuccess)
{
    const auto https_seed_url = "https://example.com/"_t;
    const auto http_seed_url = "http://example.com/"_t;

    std::vector<String> called;

    const auto result = RunHttpsFirstWithHttpFallback(
        https_seed_url, http_seed_url, [&](const String &seed_url) {
            called.push_back(seed_url);
            if (seed_url == https_seed_url)
                return MakeNoResponseFailureAttempt();
            EXPECT_EQ(seed_url, http_seed_url);
            return MakeSuccessAttempt();
        }
    );

    EXPECT_EQ(result.outcome, RunOutcome::kSucceeded);
    ASSERT_TRUE(result.http_attempt);
    ASSERT_EQ(called.size(), 2);
    EXPECT_EQ(called[0], https_seed_url);
    EXPECT_EQ(called[1], http_seed_url);
}

UTEST(CrawlerFallback, HttpsNoResponseFallsBackToHttpFailure)
{
    const auto https_seed_url = "https://example.com/"_t;
    const auto http_seed_url = "http://example.com/"_t;

    std::vector<String> called;

    const auto result = RunHttpsFirstWithHttpFallback(
        https_seed_url, http_seed_url, [&](const String &seed_url) {
            called.push_back(seed_url);
            if (seed_url == https_seed_url)
                return MakeNoResponseFailureAttempt();
            EXPECT_EQ(seed_url, http_seed_url);
            return MakeNoResponseFailureAttempt();
        }
    );

    EXPECT_EQ(result.outcome, RunOutcome::kFailed);
    ASSERT_TRUE(result.http_attempt);
    ASSERT_EQ(called.size(), 2);
    EXPECT_EQ(called[0], https_seed_url);
    EXPECT_EQ(called[1], http_seed_url);
}
