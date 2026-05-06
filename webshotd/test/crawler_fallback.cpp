#include "crawler/fallback.hpp"

#include <userver/utest/utest.hpp>

#include <optional>
#include <utility>

using namespace ws::crawler;
using namespace text::literals;

namespace {

[[nodiscard]] CrawlerError MakeNavigationError(std::optional<SeedProbe> seed_probe)
{
    return CrawlerError{
        .kind = CrawlerErrorKind::kNavigation,
        .detail = "navigation error"_t,
        .seed_probe = std::move(seed_probe),
        .cgroup_stats = {},
        .process_status = {},
    };
}

} // namespace

UTEST(CrawlerFallback, NoResponseNavigationErrorRetriesWithHttp)
{
    const auto error = MakeNavigationError(SeedProbe{.status = 0, .load_state = 0});

    EXPECT_TRUE(ShouldRetryWithHttp(error));
}

UTEST(CrawlerFallback, HttpErrorResponseDoesNotRetryWithHttp)
{
    const auto error = MakeNavigationError(SeedProbe{.status = 404, .load_state = 0});

    EXPECT_FALSE(ShouldRetryWithHttp(error));
}

UTEST(CrawlerFallback, NonNavigationErrorDoesNotRetryWithHttp)
{
    const CrawlerError error{
        .kind = CrawlerErrorKind::kProxy,
        .detail = "proxy error"_t,
        .seed_probe = SeedProbe{.status = 0, .load_state = 0},
        .cgroup_stats = {},
        .process_status = {},
    };

    EXPECT_FALSE(ShouldRetryWithHttp(error));
}
