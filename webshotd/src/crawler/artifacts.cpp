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
    String pageId;
    std::optional<String> resourceType;
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
        redirect.redirectUrl,   "GET"_t,          {}, "Document"_t,       redirect.statusCode,
        redirect.statusMessage, redirect.headers, {}, redirect.timestamp,
    };
}

[[nodiscard]] SerializableResponse makeMainDocumentResponse(const CapturedExchange &exchange)
{
    return {
        exchange.finalUrl,   "GET"_t,
        exchange.pageId,     "Document"_t,
        exchange.statusCode, exchange.statusMessage,
        exchange.headers,    exchange.body,
        exchange.timestamp,
    };
}

[[nodiscard]] SerializableResponse makeResourceResponse(const CapturedResource &resource)
{
    return {
        resource.resourceUrl,  resource.method,     {},
        resource.resourceType, resource.statusCode, resource.statusMessage,
        resource.headers,      resource.body,       resource.timestamp,
    };
}

[[nodiscard]] std::string
contentTypeForHeaders(const std::unordered_map<std::string, std::string> &headers)
{
    if (const auto it = headers.find("content-type"); it != std::end(headers))
        return it->second;
    return "application/octet-stream";
}

[[nodiscard]] String pageTimestamp(const CapturedExchange &exchange)
{
    if (!exchange.mainDocumentRedirects.empty())
        return exchange.mainDocumentRedirects.front().timestamp;
    return exchange.timestamp;
}

[[nodiscard]] std::vector<SerializableResponse>
collectSerializableResponses(const CapturedExchange &exchange)
{
    std::vector<SerializableResponse> responses;
    responses.reserve(exchange.mainDocumentRedirects.size() + exchange.resources.size() + 1);

    for (const auto &redirect : exchange.mainDocumentRedirects) {
        auto response = makeRedirectResponse(redirect);
        response.pageId = exchange.pageId;
        responses.push_back(std::move(response));
    }
    responses.push_back(makeMainDocumentResponse(exchange));
    for (const auto &resource : exchange.resources) {
        auto response = makeResourceResponse(resource);
        response.pageId = exchange.pageId;
        responses.push_back(std::move(response));
    }

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
        if (const auto maybeUrl = Url::fromText(urlText.value())) {
            requestPath = maybeUrl->pathWithSearch();
            requestHost = maybeUrl->host();
        }
    }
    const auto statusMessage =
        response.statusMessage.empty()
            ? String::fromBytesThrow(
                  std::string(
                      http::StatusCodeString(numericCast<http::StatusCode>(response.statusCode))
                  )
              )
            : response.statusMessage;

    std::string httpResponseHead = fmt::format(
        "HTTP/1.1 {} {}\r\n", response.statusCode, statusMessage
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
        "{}"
        "{}"
        "Content-Type: application/http; msgtype=response\r\n"
        "Content-Length: {}\r\n"
        "\r\n"
        "{}",
        response.responseUrl, recordDate, responseRecordId,
        response.pageId.empty() ? "" : fmt::format("WARC-Page-ID: {}\r\n", response.pageId),
        response.resourceType
            ? fmt::format("WARC-Resource-Type: {}\r\n", response.resourceType.value())
            : std::string{},
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
        "{}"
        "{}"
        "Content-Type: application/http; msgtype=request\r\n"
        "Content-Length: {}\r\n"
        "\r\n"
        "{}\r\n",
        response.responseUrl, recordDate, requestRecordId, responseRecordId,
        response.pageId.empty() ? "" : fmt::format("WARC-Page-ID: {}\r\n", response.pageId),
        response.resourceType
            ? fmt::format("WARC-Resource-Type: {}\r\n", response.resourceType.value())
            : std::string{},
        requestPayload.size(), requestPayload
    );

    responseHeader += response.body;
    responseHeader += "\r\n\r\n";
    return {std::move(responseHeader), std::move(requestHeader)};
}

[[nodiscard]] std::string buildPageInfoJson(const CapturedExchange &exchange)
{
    json::ValueBuilder pageInfo(json::Type::kObject);
    pageInfo["pageid"] = std::string(exchange.pageId.view());
    pageInfo["url"] = std::string(
        (exchange.seedUrl.empty() ? exchange.finalUrl : exchange.seedUrl).view()
    );
    pageInfo["ts"] = std::string(pageTimestamp(exchange).view());

    json::ValueBuilder urls(json::Type::kObject);
    const auto appendUrl = [&urls](
                               const String &url, i64 statusCode,
                               const std::unordered_map<std::string, std::string> &headers,
                               std::optional<String> resourceType
                           ) {
        json::ValueBuilder value(json::Type::kObject);
        value["status"] = raw(statusCode);
        const auto mime = contentTypeForHeaders(headers);
        if (!mime.empty())
            value["mime"] = mime;
        if (resourceType && !resourceType->empty())
            value["type"] = std::string(resourceType->view());
        urls[std::string(url.view())] = value.ExtractValue();
    };

    for (const auto &redirect : exchange.mainDocumentRedirects)
        appendUrl(redirect.redirectUrl, redirect.statusCode, redirect.headers, "Document"_t);
    appendUrl(exchange.finalUrl, exchange.statusCode, exchange.headers, "Document"_t);
    for (const auto &resource : exchange.resources) {
        appendUrl(
            resource.resourceUrl, resource.statusCode, resource.headers,
            resource.resourceType ? resource.resourceType : std::optional<String>{}
        );
    }
    pageInfo["urls"] = urls.ExtractValue();

    json::ValueBuilder counts(json::Type::kObject);
    counts["jsErrors"] = 0;
    pageInfo["counts"] = counts.ExtractValue();
    return json::ToString(pageInfo.ExtractValue());
}

[[nodiscard]] std::string serializePageInfoRecord(const CapturedExchange &exchange)
{
    const auto pageUrl = exchange.seedUrl.empty() ? exchange.finalUrl : exchange.seedUrl;
    const auto pageInfoUrl = fmt::format("urn:pageinfo:{}", pageUrl);
    const auto recordId = fmt::format("urn:uuid:{}", us::utils::generators::GenerateBoostUuid());
    const auto payload = buildPageInfoJson(exchange);

    return fmt::format(
        "WARC/1.1\r\n"
        "WARC-Type: resource\r\n"
        "WARC-Target-URI: {}\r\n"
        "WARC-Date: {}\r\n"
        "WARC-Record-ID: <{}>\r\n"
        "WARC-Page-ID: {}\r\n"
        "WARC-Resource-Type: pageinfo\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: {}\r\n"
        "\r\n"
        "{}\r\n\r\n",
        pageInfoUrl, pageTimestamp(exchange), recordId, exchange.pageId, payload.size(), payload
    );
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
    if (shouldIncludePort(maybeUrl.value()))
        surtHost += ":" + port;
    return String::fromBytesThrow(surtHost + ")" + path);
}

[[nodiscard]] dto::WaczIndexEntry makeWaczIndexEntry(const WarcCdxRecord &record)
{
    dto::WaczIndexEntry recordEntry;
    recordEntry.url = std::string(record.recordUrl.view());
    recordEntry.status = fmt::to_string(record.statusCode);
    recordEntry.mime = contentTypeForHeaders(record.headers);
    recordEntry.filename = "archive/data.warc";
    recordEntry.length = fmt::to_string(record.length);
    recordEntry.offset = fmt::to_string(record.offset);
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
    json::ValueBuilder pagesHeader(json::Type::kObject);
    pagesHeader["format"] = "json-pages-1.0";
    pagesHeader["id"] = "pages";
    pagesHeader["title"] = "All Pages";
    pagesHeader["hasText"] = false;
    return json::ToString(pagesHeader.ExtractValue()) + "\n" + std::string(pagesJsonl);
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
    entry.id = std::string(exchange.pageId.view());
    entry.url = std::string(
        (exchange.seedUrl.empty() ? exchange.finalUrl : exchange.seedUrl).view()
    );
    entry.title = exchange.title ? std::string(exchange.title->view())
                                 : extractHtmlTitle(exchange.body);
    entry.loadState = exchange.statusCode >= 200_i64 && exchange.statusCode < 400_i64 ? 2 : 0;
    entry.mime = contentTypeForHeaders(exchange.headers);
    entry.seed = true;
    entry.ts = datetime::TimePointTz(
        datetime::FromRfc3339StringSaturating(std::string(pageTimestamp(exchange).view()))
    );
    entry.status = raw(exchange.statusCode);
    entry.depth = 0;
    return toJsonString(entry) + "\n";
}

std::string buildSuccessStdoutLog(
    const RunRequest &run, const CapturedExchange &exchange, i64 browserPid, bool reusedBrowser
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
        "browsertrix rewrite done\n\n",
        run.seedUrl, exchange.finalUrl, exchange.statusCode,
        exchange.redirectChain.empty() ? 0 : exchange.redirectChain.size() - 1, "chromium",
        browserPid, reusedBrowser ? "true" : "false"
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

    const auto pageInfoUrl = text::format(
        "urn:pageinfo:{}", exchange.seedUrl.empty() ? exchange.finalUrl : exchange.seedUrl
    );
    auto pageInfoHeaders = std::unordered_map<std::string, std::string>{
        {"content-type", "application/json"},
    };
    const auto pageInfoBytes = serializePageInfoRecord(exchange);
    out.cdxRecords.push_back({
        pageInfoUrl,
        toCdxTimestamp(pageTimestamp(exchange)),
        200_i64,
        pageInfoHeaders,
        offset,
        i64(pageInfoBytes.size()),
    });
    out.bytes += pageInfoBytes;
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
        if (!zip.addStoredFile(path, error, body))
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
    return zipBytes.value();
}

} // namespace v1::crawler
