#pragma once

#include "crawler/cgroup_stats.hpp"
#include "text.hpp"

#include <cstdint>
#include <optional>

namespace ws::crawler {

struct [[nodiscard]] SeedProbe {
    std::optional<int64_t> status;
    std::optional<int64_t> load_state;
};

enum class CrawlerErrorKind {
    kBrowserLaunch,
    kDevtoolsStartup,
    kCdp,
    kNavigation,
    kProxy,
    kArchiveSizeLimit,
    kArchiveBuild,
    kBrowserResource,
    kNoArchiveProduced,
    kCancelled,
    kInternal,
};

struct [[nodiscard]] RawProcessStatus {
    bool exited{};
    std::optional<int> status;
};

struct [[nodiscard]] CrawlerError {
    CrawlerErrorKind kind;
    std::optional<String> detail;
    std::optional<SeedProbe> seed_probe;
    std::optional<CgroupStats> cgroup_stats;
    std::optional<RawProcessStatus> process_status;
};

[[nodiscard]] inline bool IsNoResponseSeedError(const std::optional<SeedProbe> &probe) noexcept
{
    if (!probe)
        return false;

    const auto status = probe->status ? *probe->status : 0;

    if (status == 502)
        return true;

    if (!probe->load_state)
        return false;
    if (*probe->load_state != 0)
        return false;

    if (status >= 400)
        return false;
    return status == 0;
}

[[nodiscard]] inline bool ShouldRetryWithHttp(const CrawlerError &https_error) noexcept
{
    if (https_error.kind != CrawlerErrorKind::kNavigation)
        return false;
    return IsNoResponseSeedError(https_error.seed_probe);
}

[[nodiscard]] inline String CrawlerErrorKindText(CrawlerErrorKind kind)
{
    using enum CrawlerErrorKind;
    using text::literals::operator""_t;

    switch (kind) {
    case kBrowserLaunch:
        return "browser launch error"_t;
    case kDevtoolsStartup:
        return "devtools startup error"_t;
    case kCdp:
        return "cdp error"_t;
    case kNavigation:
        return "navigation error"_t;
    case kProxy:
        return "proxy error"_t;
    case kArchiveSizeLimit:
        return "archive size limit error"_t;
    case kArchiveBuild:
        return "archive build error"_t;
    case kBrowserResource:
        return "browser resource error"_t;
    case kNoArchiveProduced:
        return "archive output error"_t;
    case kCancelled:
        return "crawler cancelled"_t;
    case kInternal:
        return "internal crawler error"_t;
    }
}

} // namespace ws::crawler
