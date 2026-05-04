#pragma once

#include "text.hpp"

#include <cstdint>
#include <optional>

#include <userver/utils/underlying_value.hpp>

namespace ws::crawler {

namespace us = userver;
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
    std::optional<int64_t> load_state;
};

struct [[nodiscard]] AttemptSummary {
    bool exited;
    int exit_code;
    bool wacz_exists;
    std::optional<SeedPageProbe> seed_probe;
    std::optional<String> failure_detail;
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
    AttemptSummary https_attempt;
    std::optional<AttemptSummary> http_attempt;
};

[[nodiscard]] inline bool
IsNoResponseSeedFailure(const std::optional<SeedPageProbe> &probe) noexcept
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

[[nodiscard]] inline bool IsNonRetryableCrawlerExitCode(int code) noexcept
{
    using enum CrawlerExitCode;
    using us::utils::UnderlyingValue;

    switch (code) {
    case UnderlyingValue(kOutOfSpace):
    case UnderlyingValue(kBrowserCrashed):
    case UnderlyingValue(kSignalInterrupted):
    case UnderlyingValue(kFailedLimit):
    case UnderlyingValue(kSignalInterruptedForce):
    case UnderlyingValue(kSizeLimit):
    case UnderlyingValue(kTimeLimit):
    case UnderlyingValue(kDiskUtilization):
    case UnderlyingValue(kFatal):
    case UnderlyingValue(kProxyError):
    case UnderlyingValue(kUploadFailed):
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline String CrawlerFailureReason(int code)
{
    using enum CrawlerExitCode;
    using text::literals::operator""_t;
    using us::utils::UnderlyingValue;

    switch (code) {
    case UnderlyingValue(kSizeLimit):
        return "crawler exceeded configured archive size limit"_t;
    case UnderlyingValue(kTimeLimit):
        return "crawler hit Browsertrix timeLimit (max crawl duration)"_t;
    case UnderlyingValue(kDiskUtilization):
        return "crawler hit Browsertrix diskUtilization limit"_t;
    case UnderlyingValue(kOutOfSpace):
        return "crawler hit out-of-space condition"_t;
    case UnderlyingValue(kProxyError):
        return "crawler failed due to proxy error"_t;
    case UnderlyingValue(kUploadFailed):
        return "crawler failed during archive upload"_t;
    case UnderlyingValue(kBrowserCrashed):
        return "crawler browser crashed"_t;
    case UnderlyingValue(kFatal):
        return "crawler hit fatal error"_t;
    default:
        return "crawler failed"_t;
    }
}

[[nodiscard]] inline bool IsAttemptSuccess(const AttemptSummary &attempt) noexcept
{
    using enum CrawlerExitCode;
    using us::utils::UnderlyingValue;

    return attempt.exited && attempt.exit_code == UnderlyingValue(kSuccess) && attempt.wacz_exists;
}

[[nodiscard]] inline bool ShouldAttemptHttpFallback(const AttemptSummary &https_attempt) noexcept
{
    using enum CrawlerExitCode;
    using us::utils::UnderlyingValue;

    if (!https_attempt.exited)
        return false;
    if (https_attempt.exit_code == UnderlyingValue(kSuccess))
        return false;
    if (IsNonRetryableCrawlerExitCode(https_attempt.exit_code))
        return false;
    return IsNoResponseSeedFailure(https_attempt.seed_probe);
}

template <typename AttemptFn>
[[nodiscard]] RunResult RunHttpsFirstWithHttpFallback(
    const String &https_seed_url, const String &http_seed_url, AttemptFn &&attempt
)
{
    using enum CrawlerExitCode;
    using us::utils::UnderlyingValue;

    auto https_attempt = attempt(https_seed_url);
    if (IsAttemptSuccess(https_attempt))
        return {RunOutcome::kSucceeded, https_attempt, {}};
    if (!https_attempt.exited)
        return {RunOutcome::kFailedChildNoExit, https_attempt, {}};
    if (https_attempt.exit_code == UnderlyingValue(kSizeLimit))
        return {RunOutcome::kFailedSizeLimit, https_attempt, {}};
    if (https_attempt.exit_code == UnderlyingValue(kSuccess) && !https_attempt.wacz_exists)
        return {RunOutcome::kFailedNoWacz, https_attempt, {}};
    if (!ShouldAttemptHttpFallback(https_attempt))
        return {RunOutcome::kFailed, https_attempt, {}};

    auto http_attempt = attempt(http_seed_url);
    if (IsAttemptSuccess(http_attempt))
        return {RunOutcome::kSucceeded, https_attempt, http_attempt};
    if (!http_attempt.exited)
        return {RunOutcome::kFailedChildNoExit, https_attempt, http_attempt};
    if (http_attempt.exit_code == UnderlyingValue(kSizeLimit))
        return {RunOutcome::kFailedSizeLimit, https_attempt, http_attempt};
    if (http_attempt.exit_code == UnderlyingValue(kSuccess) && !http_attempt.wacz_exists)
        return {RunOutcome::kFailedNoWacz, https_attempt, http_attempt};
    return {RunOutcome::kFailed, https_attempt, http_attempt};
}

} // namespace ws::crawler
