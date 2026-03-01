#pragma once

#include "text.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace v1::crawler {

enum class CrawlerExitCode : int {
    kSuccess = 0,
    kOutOfSpace = 3,
    kBrowserCrashed = 10,
    kSignalInterrupted = 11,
    kFailedLimit = 12,
    kSignalInterruptedForce = 13,
    kSizeLimit = 14,
    kTimeLimit = 15,
    kDiskUtilization = 16,
    kFatal = 17,
    kProxyError = 21,
    kUploadFailed = 22,
};

struct [[nodiscard]] SeedPageProbe {
    std::optional<int64_t> status;
    std::optional<int64_t> loadState;
};

struct [[nodiscard]] AttemptSummary {
    bool exited;
    int exitCode;
    bool waczExists;
    std::optional<SeedPageProbe> seedProbe;
};

enum class RunOutcome {
    kSucceeded,
    kFailed,
    kFailedChildNoExit,
    kFailedNoWacz,
    kFailedSizeLimit,
};

struct [[nodiscard]] RunResult {
    RunOutcome outcome;
    AttemptSummary httpsAttempt;
    std::optional<AttemptSummary> httpAttempt;
};

[[nodiscard]] inline bool
isNoResponseSeedFailure(const std::optional<SeedPageProbe> &probe) noexcept
{
    if (!probe)
        return false;

    const auto status = probe->status.value_or(0);

    if (status == 502)
        return true;

    if (!probe->loadState)
        return false;
    if (*probe->loadState != 0)
        return false;

    if (status >= 400)
        return false;
    return status == 0;
}

[[nodiscard]] inline bool isNonRetryableCrawlerExitCode(int code) noexcept
{
    using enum CrawlerExitCode;
    switch (static_cast<CrawlerExitCode>(code)) {
    case kOutOfSpace:
    case kBrowserCrashed:
    case kSignalInterrupted:
    case kFailedLimit:
    case kSignalInterruptedForce:
    case kSizeLimit:
    case kTimeLimit:
    case kDiskUtilization:
    case kFatal:
    case kProxyError:
    case kUploadFailed:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline String crawlerFailureReason(int code)
{
    using enum CrawlerExitCode;
    using text::literals::operator""_t;
    switch (static_cast<CrawlerExitCode>(code)) {
    case kSizeLimit:
        return "crawler hit Browsertrix sizeLimit (max WARC size)"_t;
    case kTimeLimit:
        return "crawler hit Browsertrix timeLimit (max crawl duration)"_t;
    case kDiskUtilization:
        return "crawler hit Browsertrix diskUtilization limit"_t;
    case kOutOfSpace:
        return "crawler hit out-of-space condition"_t;
    case kProxyError:
        return "crawler failed due to proxy error"_t;
    case kUploadFailed:
        return "crawler failed during archive upload"_t;
    case kBrowserCrashed:
        return "crawler browser crashed"_t;
    case kFatal:
        return "crawler hit fatal error"_t;
    default:
        return "crawler failed"_t;
    }
}

[[nodiscard]] inline bool isAttemptSuccess(const AttemptSummary &attempt) noexcept
{
    return attempt.exited && attempt.exitCode == static_cast<int>(CrawlerExitCode::kSuccess) &&
           attempt.waczExists;
}

[[nodiscard]] inline bool shouldAttemptHttpFallback(const AttemptSummary &httpsAttempt) noexcept
{
    if (!httpsAttempt.exited)
        return false;
    if (httpsAttempt.exitCode == static_cast<int>(CrawlerExitCode::kSuccess))
        return false;
    if (isNonRetryableCrawlerExitCode(httpsAttempt.exitCode))
        return false;
    return isNoResponseSeedFailure(httpsAttempt.seedProbe);
}

template <typename AttemptFn>
[[nodiscard]] RunResult runHttpsFirstWithHttpFallback(
    const String &httpsSeedUrl, const String &httpSeedUrl, AttemptFn &&attempt
)
{
    auto httpsAttempt = attempt(httpsSeedUrl);
    if (isAttemptSuccess(httpsAttempt))
        return {RunOutcome::kSucceeded, httpsAttempt, {}};
    if (!httpsAttempt.exited)
        return {RunOutcome::kFailedChildNoExit, httpsAttempt, {}};
    if (httpsAttempt.exitCode == static_cast<int>(CrawlerExitCode::kSizeLimit))
        return {RunOutcome::kFailedSizeLimit, httpsAttempt, {}};
    if (httpsAttempt.exitCode == static_cast<int>(CrawlerExitCode::kSuccess) &&
        !httpsAttempt.waczExists)
        return {RunOutcome::kFailedNoWacz, httpsAttempt, {}};
    if (!shouldAttemptHttpFallback(httpsAttempt))
        return {RunOutcome::kFailed, httpsAttempt, {}};

    auto httpAttempt = attempt(httpSeedUrl);
    if (isAttemptSuccess(httpAttempt))
        return {RunOutcome::kSucceeded, httpsAttempt, httpAttempt};
    if (!httpAttempt.exited)
        return {RunOutcome::kFailedChildNoExit, httpsAttempt, httpAttempt};
    if (httpAttempt.exitCode == static_cast<int>(CrawlerExitCode::kSizeLimit))
        return {RunOutcome::kFailedSizeLimit, httpsAttempt, httpAttempt};
    if (httpAttempt.exitCode == static_cast<int>(CrawlerExitCode::kSuccess) &&
        !httpAttempt.waczExists)
        return {RunOutcome::kFailedNoWacz, httpsAttempt, httpAttempt};
    return {RunOutcome::kFailed, httpsAttempt, httpAttempt};
}

} // namespace v1::crawler
