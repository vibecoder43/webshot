#pragma once

#include "crawler/artifacts.hpp"
#include "crawler/fallback.hpp"
#include "crawler/limits.hpp"
#include "integers.hpp"
#include "userver_namespaces.hpp"

#include <chrono>
#include <optional>
#include <string>

#include <userver/clients/dns/resolver_fwd.hpp>
#include <userver/engine/subprocess/process_starter.hpp>

namespace v1 {
class Denylist;
class Config;
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
    std::chrono::milliseconds browserStopTimeout;
    std::chrono::milliseconds proxyStopTimeout;
    bool enableLocalFixtureRewrite;
};

} // namespace crawler

struct [[nodiscard]] CrawlerRunArtifacts {
    crawler::AttemptSummary attempt;
    std::string stdoutLog;
    std::string stderrLog;
    std::optional<std::string> wacz;
    std::optional<std::string> pagesJsonl;
    std::optional<std::string> contentSha256;
    std::optional<String> replayUrl;
};

class [[nodiscard]] CrawlerRunner final {
public:
    CrawlerRunner(
        Denylist &denylist, const Config &config, us::clients::dns::Resolver &dnsResolver,
        eng::subprocess::ProcessStarter &processStarter, std::chrono::seconds runTimeout,
        std::string stateDir, std::optional<crawler::CgroupLimits> limits, i64 maxArchiveBytes,
        crawler::CaptureTimings timings, crawler::CrawlerTunables tunables,
        i64 networkDownBytesRatioMax
    );

    [[nodiscard]] CrawlerRunArtifacts run(const String &seedUrl) const;

private:
    Denylist &denylist;
    const Config &config;
    us::clients::dns::Resolver &dnsResolver;
    eng::subprocess::ProcessStarter &processStarter;
    std::chrono::seconds runTimeout;
    std::string browserRunsRoot;
    std::string cgroupRootPath;
    std::optional<crawler::CgroupLimits> cgroupLimits;
    i64 maxArchiveBytes;
    crawler::CaptureTimings timings;
    crawler::CrawlerTunables tunables;
    i64 networkDownBytesRatioMax;
};

} // namespace v1
