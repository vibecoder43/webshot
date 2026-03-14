#include "crawler/artifacts.hpp"
#include "ip_utils.hpp"
#include "url.hpp"

#include <arkhiv/zip_archive.hpp>

#include "schema/browsertrix_pages.hpp"
#include "schema/wacz.hpp"

#include <algorithm>
#include <array>
#include <boost/uuid/uuid_io.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <userver/formats/json.hpp>
#include <userver/http/status_code.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/datetime/from_string_saturating.hpp>

namespace us = userver;
namespace json = us::formats::json;
namespace datetime = us::utils::datetime;
namespace http = us::http;

namespace v1::crawler {
using namespace text::literals;

namespace {

constexpr std::string_view kUserAgent = "webshotd/0.1.0";

[[nodiscard]] std::string extractHtmlTitle(std::string_view body)
{
    const auto openPos = body.find("<title");
    if (openPos == std::string_view::npos)
        return {};
    const auto start = body.find('>', openPos);
    if (start == std::string_view::npos)
        return {};
    const auto end = body.find("</title>", start + 1);
    if (end == std::string_view::npos)
        return {};
    return std::string(body.substr(start + 1, end - start - 1));
}

template <typename T> [[nodiscard]] std::string toJsonString(const T &value)
{
    return json::ToString(json::ValueBuilder(value).ExtractValue());
}

[[nodiscard]] String toCdxTimestamp(const String &iso)
{
    return String::fromBytesThrow(
        datetime::UtcTimestring(
            datetime::FromRfc3339StringSaturating(std::string(iso.view())), "%Y%m%d%H%M%S"
        )
    );
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeResponseHeaders(const std::unordered_map<std::string, std::string> &headers)
{
    std::unordered_map<std::string, std::string> out;
    for (const auto &[name, value] : headers) {
        if (name == "content-encoding" || name == "content-length" || name == "transfer-encoding") {
            continue;
        }
        out.emplace(name, value);
    }
    return out;
}

struct [[nodiscard]] SerializableResponse {
    String responseUrl;
    String method;
    i64 statusCode;
    String statusMessage;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    String timestamp;
};

[[nodiscard]] SerializableResponse
makeRedirectResponse(const CapturedMainDocumentRedirect &redirect)
{
    return {
        redirect.redirectUrl, "GET"_t, redirect.statusCode, redirect.statusMessage,
        redirect.headers,     {},      redirect.timestamp,
    };
}

[[nodiscard]] SerializableResponse makeMainDocumentResponse(const CapturedExchange &exchange)
{
    return {
        exchange.finalUrl, "GET"_t,       exchange.statusCode, exchange.statusMessage,
        exchange.headers,  exchange.body, exchange.timestamp,
    };
}

[[nodiscard]] SerializableResponse makeResourceResponse(const CapturedResource &resource)
{
    return {
        resource.resourceUrl, resource.method, resource.statusCode, resource.statusMessage,
        resource.headers,     resource.body,   resource.timestamp,
    };
}

[[nodiscard]] std::vector<SerializableResponse>
collectSerializableResponses(const CapturedExchange &exchange)
{
    std::vector<SerializableResponse> responses;
    responses.reserve(exchange.mainDocumentRedirects.size() + exchange.resources.size() + 1);

    for (const auto &redirect : exchange.mainDocumentRedirects)
        responses.push_back(makeRedirectResponse(redirect));
    responses.push_back(makeMainDocumentResponse(exchange));
    for (const auto &resource : exchange.resources)
        responses.push_back(makeResourceResponse(resource));

    std::sort(std::begin(responses), std::end(responses), [](const auto &left, const auto &right) {
        return left.timestamp < right.timestamp;
    });
    return responses;
}

[[nodiscard]] std::pair<std::string, std::string>
serializeRecordPair(const SerializableResponse &response)
{
    const auto recordDate = response.timestamp;
    const auto responseRecordId = fmt::format(
        "urn:uuid:{}", us::utils::generators::GenerateBoostUuid()
    );
    const auto requestRecordId = fmt::format(
        "urn:uuid:{}", us::utils::generators::GenerateBoostUuid()
    );
    const auto normalizedHeaders = normalizeResponseHeaders(response.headers);
    auto requestPath = "/"_t;
    String requestHost;
    if (const auto urlText = String::fromBytes(response.responseUrl.view())) {
        if (const auto maybeUrl = Url::fromText(*urlText)) {
            requestPath = maybeUrl->pathWithSearch();
            requestHost = maybeUrl->host();
        }
    }
    const auto statusMessage =
        response.statusMessage.empty()
            ? String::fromBytesThrow(
                  std::string(
                      http::StatusCodeString(
                          static_cast<http::StatusCode>(toNative(response.statusCode))
                      )
                  )
              )
            : response.statusMessage;

    std::string httpResponseHead = fmt::format(
        "HTTP/1.1 {} {}\r\n", toNative(response.statusCode), statusMessage
    );
    for (const auto &[name, value] : normalizedHeaders)
        httpResponseHead += fmt::format("{}: {}\r\n", name, value);
    httpResponseHead += "\r\n";

    std::string responseHeader = fmt::format(
        "WARC/1.1\r\n"
        "WARC-Type: response\r\n"
        "WARC-Target-URI: {}\r\n"
        "WARC-Date: {}\r\n"
        "WARC-Record-ID: <{}>\r\n"
        "Content-Type: application/http; msgtype=response\r\n"
        "Content-Length: {}\r\n"
        "\r\n"
        "{}",
        response.responseUrl, recordDate, responseRecordId,
        httpResponseHead.size() + response.body.size(), httpResponseHead
    );

    std::string requestPayload = fmt::format(
        "{} {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: {}\r\n\r\n",
        response.method.empty() ? "GET"_t : response.method, requestPath, requestHost, kUserAgent
    );

    std::string requestHeader = fmt::format(
        "WARC/1.1\r\n"
        "WARC-Type: request\r\n"
        "WARC-Target-URI: {}\r\n"
        "WARC-Date: {}\r\n"
        "WARC-Record-ID: <{}>\r\n"
        "WARC-Concurrent-To: <{}>\r\n"
        "Content-Type: application/http; msgtype=request\r\n"
        "Content-Length: {}\r\n"
        "\r\n"
        "{}\r\n",
        response.responseUrl, recordDate, requestRecordId, responseRecordId, requestPayload.size(),
        requestPayload
    );

    responseHeader += response.body;
    responseHeader += "\r\n\r\n";
    return {std::move(responseHeader), std::move(requestHeader)};
}

[[nodiscard]] bool shouldIncludePort(const Url &url)
{
    if (!url.hasPort())
        return false;

    const auto defaultPort = ada::scheme::get_special_port(url.schemeType());
    if (defaultPort == 0)
        return true;

    return url.port().view() != std::to_string(defaultPort);
}

[[nodiscard]] String toSurtKey(const String &urlText)
{
    const auto maybeUrl = Url::fromText(urlText);
    if (!maybeUrl)
        return urlText;

    if (!maybeUrl->isHttp() && !maybeUrl->isHttps())
        return urlText;

    std::string host(maybeUrl->hostname().view());
    std::string port(maybeUrl->port().view());
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    if (isIpLiteralHostname(host))
        return urlText;

    std::vector<std::string> parts;
    size_t offset = 0;
    while (offset <= host.size()) {
        const auto next = host.find('.', offset);
        if (next == std::string::npos) {
            parts.emplace_back(host.substr(offset));
            break;
        }
        parts.emplace_back(host.substr(offset, next - offset));
        offset = next + 1;
    }
    std::reverse(std::begin(parts), std::end(parts));

    std::string surtHost;
    for (size_t index = 0; index < parts.size(); index++) {
        if (index > 0)
            surtHost += ",";
        surtHost += parts[index];
    }

    auto path = std::string(maybeUrl->pathWithSearch().view());
    if (shouldIncludePort(*maybeUrl))
        surtHost += ":" + port;
    return String::fromBytesThrow(surtHost + ")" + path);
}

[[nodiscard]] dto::WaczIndexEntry makeWaczIndexEntry(const WarcCdxRecord &record)
{
    dto::WaczIndexEntry recordEntry;
    recordEntry.url = std::string(record.recordUrl.view());
    recordEntry.status = std::to_string(toNative(record.statusCode));
    recordEntry.mime = record.headers.count("content-type") ? record.headers.at("content-type")
                                                            : "application/octet-stream";
    recordEntry.filename = "data.warc";
    recordEntry.length = std::to_string(toNative(record.length));
    recordEntry.offset = std::to_string(toNative(record.offset));
    return recordEntry;
}

[[nodiscard]] std::string buildCdxj(const std::vector<WarcCdxRecord> &records)
{
    std::string cdxj;
    for (const auto &record : records) {
        cdxj += toSurtKey(record.recordUrl).view();
        cdxj += " ";
        cdxj += record.timestamp.view();
        cdxj += " ";
        cdxj += toJsonString(makeWaczIndexEntry(record));
        cdxj += "\n";
    }
    return cdxj;
}

[[nodiscard]] std::string buildWaczPages(std::string_view pagesJsonl)
{
    dto::WaczPagesHeader pagesHeader;
    pagesHeader.format = "json-pages-1.0";
    pagesHeader.id = "pages";
    pagesHeader.title = "Seed Pages";
    pagesHeader.hasText = false;
    return toJsonString(pagesHeader) + "\n" + std::string(pagesJsonl);
}

[[nodiscard]] dto::WaczResource
makeWaczResource(std::string_view name, std::string_view path, size_t bytes)
{
    dto::WaczResource item;
    item.name = std::string(name);
    item.path = std::string(path);
    item.bytes = numericCast<int64_t>(bytes);
    return item;
}

[[nodiscard]] std::vector<dto::WaczResource> buildWaczResources(
    size_t warcBytes, size_t pagesBytes, size_t stdoutBytes, size_t stderrBytes, size_t cdxjBytes
)
{
    struct ResourceSpec {
        std::string_view name;
        std::string_view path;
        size_t bytes;
    };

    std::vector<dto::WaczResource> resources;
    resources.reserve(5);
    const auto specs = std::array<ResourceSpec, 5>{{
        {"archive", "archive/data.warc", warcBytes},
        {"pages", "pages/pages.jsonl", pagesBytes},
        {"stdout log", "logs/stdout.log", stdoutBytes},
        {"stderr log", "logs/stderr.log", stderrBytes},
        {"index", "indexes/index.cdxj", cdxjBytes},
    }};
    for (const auto &spec : specs)
        resources.push_back(makeWaczResource(spec.name, spec.path, spec.bytes));
    return resources;
}

[[nodiscard]] dto::WaczDataPackage
buildWaczDataPackage(const RunRequest &run, std::vector<dto::WaczResource> resources)
{
    dto::WaczDataPackage datapackage;
    datapackage.profile = "data-package";
    datapackage.created = datetime::TimePointTz(datetime::Now());
    datapackage.wacz_version = "1.1.1";
    datapackage.software = "webshotd";
    datapackage.title = std::string(run.seedUrl.view());
    datapackage.resources = std::move(resources);
    return datapackage;
}

} // namespace

std::string buildPagesJsonl(const CapturedExchange &exchange)
{
    dto::BrowsertrixPageEntry entry;
    entry.id = boost::uuids::to_string(us::utils::generators::GenerateBoostUuid());
    entry.url = std::string(exchange.finalUrl.view());
    entry.title = exchange.title ? std::string(exchange.title->view())
                                 : extractHtmlTitle(exchange.body);
    entry.loadState = toNative(
        exchange.statusCode >= 200_i64 && exchange.statusCode < 400_i64 ? 2_i64 : 0_i64
    );
    entry.mime = exchange.headers.count("content-type") ? exchange.headers.at("content-type") : "";
    entry.seed = true;
    entry.ts = datetime::TimePointTz(
        datetime::FromRfc3339StringSaturating(std::string(exchange.timestamp.view()))
    );
    entry.status = toNative(exchange.statusCode);
    entry.depth = 0;
    return toJsonString(entry) + "\n";
}

std::string buildSuccessStdoutLog(
    const RunRequest &run, const CapturedExchange &exchange, const std::string &browserBin,
    const std::optional<String> &geometry, i64 browserPid, bool reusedBrowser
)
{
    return fmt::format(
        "browsertrix rewrite start\n"
        "seed_url={}\n"
        "final_url={}\n"
        "status={}\n"
        "redirects={}\n"
        "browser_bin={}\n"
        "browser_pid={}\n"
        "reused_browser={}\n"
        "{}"
        "browsertrix rewrite done\n\n",
        run.seedUrl, exchange.finalUrl, toNative(exchange.statusCode),
        exchange.redirectChain.empty() ? 0 : exchange.redirectChain.size() - 1, browserBin,
        toNative(browserPid), reusedBrowser ? "true" : "false",
        geometry ? fmt::format("geometry={}\n", *geometry) : ""
    );
}

WarcBuildOutput buildWarc(const CapturedExchange &exchange)
{
    const auto responses = collectSerializableResponses(exchange);

    WarcBuildOutput out;
    auto offset = 0_i64;
    for (const auto &response : responses) {
        auto [responseBytes, requestBytes] = serializeRecordPair(response);
        out.cdxRecords.push_back({
            response.responseUrl,
            toCdxTimestamp(response.timestamp),
            response.statusCode,
            response.headers,
            offset,
            i64(responseBytes.size()),
        });
        out.bytes += responseBytes;
        out.bytes += requestBytes;
        offset += i64(responseBytes.size() + requestBytes.size());
    }
    return out;
}

std::string buildWacz(
    const RunRequest &run, const std::string &pagesJsonl, const WarcBuildOutput &warc,
    const std::string &stdoutLog, const std::string &stderrLog
)
{
    const auto cdxj = buildCdxj(warc.cdxRecords);
    const auto waczPages = buildWaczPages(pagesJsonl);
    auto resources = buildWaczResources(
        warc.bytes.size(), waczPages.size(), stdoutLog.size(), stderrLog.size(), cdxj.size()
    );
    const auto datapackageJson = toJsonString(buildWaczDataPackage(run, std::move(resources)));

    arkhiv::ZipArchiveBuilder zip;
    arkhiv::ZipArchiveError error;
    const auto addFileOrThrow = [&error, &zip](std::string_view path, std::string_view body) {
        if (!zip.addStoredFile(path, body, error))
            throw std::runtime_error(error.detail);
    };

    addFileOrThrow("datapackage.json", datapackageJson);
    addFileOrThrow("archive/data.warc", warc.bytes);
    addFileOrThrow("pages/pages.jsonl", waczPages);
    addFileOrThrow("logs/stdout.log", stdoutLog);
    addFileOrThrow("logs/stderr.log", stderrLog);
    addFileOrThrow("indexes/index.cdxj", cdxj);

    const auto zipBytes = zip.finish(error);
    if (!zipBytes)
        throw std::runtime_error(error.detail);
    return *zipBytes;
}

} // namespace v1::crawler
