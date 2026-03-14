#pragma once

#include "crawler/artifacts.hpp"
#include "crawler/fallback.hpp"
#include "integers.hpp"

#include <optional>
#include <string>

#include <userver/clients/http/client.hpp>
#include <userver/engine/subprocess/process_starter.hpp>

namespace us = userver;

namespace v1 {

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
        us::engine::subprocess::ProcessStarter &processStarter, i64 runTimeoutSec
    );

    CrawlerRunner(
        us::clients::http::Client &httpClient,
        us::engine::subprocess::ProcessStarter &processStarter, i64 runTimeoutSec,
        String proxyServer, std::string browserBin, String geometry
    );

    [[nodiscard]] CrawlerRunArtifacts run(const String &seedUrl) const;

private:
    us::clients::http::Client &httpClient;
    us::engine::subprocess::ProcessStarter &processStarter;
    i64 runTimeoutSec;
    String proxyServer;
    std::string browserBin;
    String geometry;
};

} // namespace v1
