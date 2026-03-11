#include "crawlerd_client.hpp"

#include "integers.hpp"
#include "schema/crawlerd.hpp"
#include "text.hpp"

#include <arkhiv/tar_archive.hpp>

#include <chrono>
#include <exception>
#include <limits>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <userver/clients/http/response.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/content_type.hpp>
#include <userver/http/status_code.hpp>
#include <userver/utils/assert.hpp>

using namespace text::literals;

namespace json = userver::formats::json;
namespace http = userver::http;

namespace v1 {
namespace {

[[nodiscard]] bool hasContentTypePrefix(std::string_view actual, std::string_view expected) noexcept
{
    return actual.substr(0, expected.size()) == expected;
}

[[nodiscard]] std::string
buildNonSuccessStatusMessage(const us::clients::http::Response &response, std::string_view body)
{
    try {
        const auto value = json::FromString(body);
        const auto errorValue = value["error"];
        if (errorValue.IsMissing())
            return fmt::format("crawlerd returned HTTP {}", response.status_code());

        const auto messageValue = errorValue["message"];
        if (messageValue.IsMissing())
            return fmt::format("crawlerd returned HTTP {}", response.status_code());

        return fmt::format(
            "crawlerd returned HTTP {}: {}", response.status_code(), messageValue.As<std::string>()
        );
    } catch (const std::exception &) {
        return fmt::format("crawlerd returned HTTP {}", response.status_code());
    }
}

[[nodiscard]] std::string_view
requireTarEntry(const arkhiv::TarArchive &archive, std::string_view path)
{
    const auto file = archive.findFile(path);
    if (!file)
        throw std::runtime_error(fmt::format("crawlerd tar response missing {}", path));
    return *file;
}

[[nodiscard]] std::optional<crawler::SeedPageProbe>
convertSeedProbe(const std::optional<dto::CrawlerRunResult::Seed_Probe> &seedProbe)
{
    if (!seedProbe)
        return {};

    crawler::SeedPageProbe out;
    out.status = seedProbe->status;
    out.loadState = seedProbe->load_state;
    return out;
}

[[nodiscard]] crawler::AttemptSummary
buildAttemptSummary(const dto::CrawlerRunResult &result, bool waczExists)
{
    crawler::AttemptSummary attempt;
    attempt.exited = true;
    attempt.exitCode = result.exit_code;
    attempt.waczExists = waczExists;
    attempt.seedProbe = convertSeedProbe(result.seed_probe);
    if (result.failure_detail)
        attempt.failureDetail = String::fromBytesThrow(*result.failure_detail);
    return attempt;
}

[[nodiscard]] CrawlerRunArtifacts parseTarRunResponse(const us::clients::http::Response &response)
{
    if (!response.headers().contains(http::headers::kContentType)) {
        throw std::runtime_error("crawlerd tar response missing Content-Type");
    }

    const auto &contentType = response.headers().at(http::headers::kContentType);
    if (!hasContentTypePrefix(contentType, "application/x-tar")) {
        throw std::runtime_error(
            fmt::format("crawlerd returned unexpected Content-Type {}", contentType)
        );
    }

    arkhiv::TarArchiveError tarError;
    const auto archive = arkhiv::TarArchive::fromBytes(response.body_view(), tarError);
    if (!archive) {
        throw std::runtime_error(
            fmt::format(
                "failed to parse crawlerd tar response: {}",
                tarError.detail.empty() ? "invalid tar archive" : tarError.detail
            )
        );
    }

    const auto resultJson = requireTarEntry(*archive, "result.json");
    const auto stdoutLog = requireTarEntry(*archive, "stdout.log");
    const auto stderrLog = requireTarEntry(*archive, "stderr.log");

    dto::CrawlerRunResult result;
    try {
        result = json::FromString(resultJson).As<dto::CrawlerRunResult>();
    } catch (const std::exception &e) {
        throw std::runtime_error(fmt::format("invalid crawlerd result.json: {}", e.what()));
    }

    const auto wacz = archive->findFile("capture.wacz");
    const auto pagesJsonl = archive->findFile("pages.jsonl");

    if (result.status == dto::CrawlerRunResult::Status::kSucceeded && !wacz) {
        throw std::runtime_error("successful crawlerd run missing capture.wacz");
    }

    CrawlerRunArtifacts out;
    out.attempt = buildAttemptSummary(result, wacz.has_value());
    out.stdoutLog = std::string(stdoutLog);
    out.stderrLog = std::string(stderrLog);
    if (wacz)
        out.wacz = std::string(*wacz);
    if (pagesJsonl)
        out.pagesJsonl = std::string(*pagesJsonl);
    return out;
}

} // namespace

CrawlerdClient::CrawlerdClient(
    us::clients::http::Client &httpClientIn, String baseUrlIn, String socketPathIn,
    i64 runTimeoutSecIn
)
    : httpClient(httpClientIn), baseUrl(std::move(baseUrlIn)), socketPath(std::move(socketPathIn)),
      runTimeoutSec(runTimeoutSecIn)
{
}

CrawlerRunArtifacts CrawlerdClient::run(const String &seedUrl) const
{
    constexpr auto kMillisPerSecond = 1000_i64;

    dto::CrawlerRunRequest requestBody;
    requestBody.url = std::string(seedUrl.view());
    const auto jobTimeoutMs = runTimeoutSec * kMillisPerSecond;
    UINVARIANT(
        jobTimeoutMs <= i64(std::numeric_limits<int>::max()),
        "crawlerd timeout_ms exceeds supported range"
    );
    requestBody.timeout_ms = toNative(i32(jobTimeoutMs));

    auto requestJson = json::ToString(json::ValueBuilder(requestBody).ExtractValue());
    auto url = fmt::format("{}/run", baseUrl);
    auto socketPathString = std::string(socketPath.view());

    us::clients::http::Headers headers;
    headers.insert_or_assign(
        http::headers::kContentType, http::content_type::kApplicationJson.ToString()
    );

    const auto response = httpClient.CreateRequest()
                              .post(std::move(url), std::move(requestJson))
                              .headers(headers)
                              .unix_socket_path(socketPathString.c_str())
                              .timeout(toSeconds(runTimeoutSec))
                              .follow_redirects(false)
                              .SetDestinationMetricName("crawlerd.run")
                              .perform();

    if (!response->IsOk())
        throw std::runtime_error(buildNonSuccessStatusMessage(*response, response->body_view()));

    return parseTarRunResponse(*response);
}

} // namespace v1
