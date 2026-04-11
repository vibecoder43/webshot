#pragma once

#include "crawler/artifacts.hpp"
#include "crawler/fallback.hpp"
#include "crawler/limits.hpp"
#include "integers.hpp"

#include <chrono>
#include <optional>
#include <string>

#include <userver/clients/http/client.hpp>
#include <userver/engine/subprocess/process_starter.hpp>

namespace us = userver;

namespace v1 {
namespace crawler {

struct [[nodiscard]] CaptureTimings {
    std::chrono::seconds postLoadDelay;
    std::chrono::seconds netIdleWait;
    std::chrono::seconds pageExtraDelay;
    std::chrono::seconds behaviorTimeout;
};

struct [[nodiscard]] CrawlerTunables {
    std::chrono::seconds devtoolsStartupTimeout;
    std::chrono::seconds cdpHandshakeTimeout;
    std::chrono::seconds cdpCommandTimeout;
    std::chrono::milliseconds devtoolsPollInterval;
    std::chrono::milliseconds cdpWaitPollInterval;
    std::chrono::milliseconds browserStopTimeout;
    std::chrono::milliseconds proxyStopTimeout;
};

} // namespace crawler

struct [[nodiscard]] CrawlerRunArtifacts {
    crawler::AttemptSummary attempt;
    std::string stdoutLog;
    std::string stderrLog;
    std::optional<std::string> wacz;
    std::optional<std::string> pagesJsonl;
};

class [[nodiscard]] CrawlerRunner final {
public:
    CrawlerRunner(
        us::clients::http::Client &httpClient,
        us::engine::subprocess::ProcessStarter &processStarter, std::chrono::seconds runTimeout,
        std::string stateDir, std::optional<crawler::CgroupLimits> limits,
        crawler::CaptureTimings timings, crawler::CrawlerTunables tunables
    );

    [[nodiscard]] CrawlerRunArtifacts run(const String &seedUrl) const;

private:
    us::clients::http::Client &httpClient;
    us::engine::subprocess::ProcessStarter &processStarter;
    std::chrono::seconds runTimeout;
    std::string browserRunsRoot;
    std::string cgroupRootPath;
    std::optional<crawler::CgroupLimits> cgroupLimits;
    crawler::CaptureTimings timings;
    crawler::CrawlerTunables tunables;
};

} // namespace v1
