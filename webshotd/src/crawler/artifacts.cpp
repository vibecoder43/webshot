#include "crawler/artifacts.hpp"
#include "url.hpp"
#include "userver_namespaces.hpp"

#include <arkhiv/gzip.hpp>
#include <arkhiv/zip_archive.hpp>

#include "schema/browsertrix_pages.hpp"
#include "schema/wacz.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

#include "uuid_format.hpp"

#include <userver/crypto/hash.hpp>
#include <userver/formats/json.hpp>
#include <userver/http/status_code.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/datetime/from_string_saturating.hpp>
namespace http = userver::http;

namespace v1::crawler {
using namespace text::literals;

namespace {

constexpr std::string_view kUserAgent = "webshotd/0.1.0";
constexpr std::string_view kWarcPath = "archive/data.warc.gz";
constexpr std::string_view kWarcFilename = "data.warc.gz";
constexpr std::string_view kIndexPath = "indexes/index.cdx";

[[nodiscard]] std::string sha256Bytes(std::string_view data)
{
    return userver::crypto::hash::Sha256(data, userver::crypto::hash::OutputEncoding::kBinary);
}

[[nodiscard]] std::string sha256Bytes(std::initializer_list<std::string_view> data)
{
    return userver::crypto::hash::Sha256(data, userver::crypto::hash::OutputEncoding::kBinary);
}

[[nodiscard]] std::string sha256Hex(std::string_view data)
{
    return userver::crypto::hash::Sha256(data, userver::crypto::hash::OutputEncoding::kHex);
}

[[nodiscard]] std::string sha256PrefixedHex(std::string_view data, std::string_view prefix)
{
    return std::format("{}{}", prefix, sha256Hex(data));
}

[[nodiscard]] Expected<std::string, ArtifactFailure> gzipMember(std::string_view body) noexcept
{
    arkhiv::GzipError error;
    auto maybeBytes = arkhiv::gzipCompressMember(body, error);
    if (!maybeBytes)
        return std::unexpected(
            ArtifactFailure{.code = ArtifactError::kGzipFailed, .detail = error.detail}
        );
    return std::move(*maybeBytes);
}

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
    return std::string{body.substr(start + 1, end - start - 1)};
}

template <typename T> [[nodiscard]] std::string toJsonString(const T &value)
{
    return json::ToString(json::ValueBuilder(value).ExtractValue());
}

[[nodiscard]] String toCdxTimestamp(const String &iso)
{
    return String::fromBytes(
               datetime::UtcTimestring(
                   datetime::FromRfc3339StringSaturating(std::string(iso.view())), "%Y%m%d%H%M%S"
               )
    )
        .expect();
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
    i64 statusCode{0};
    String statusMessage;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    String timestamp;
};

[[nodiscard]] SerializableResponse
makeRedirectResponse(const CapturedMainDocumentRedirect &redirect)
{
    return SerializableResponse{
        .responseUrl = redirect.redirectUrl,
        .method = "GET"_t,
        .pageId = {},
        .resourceType = "Document"_t,
        .statusCode = redirect.statusCode,
        .statusMessage = redirect.statusMessage,
        .headers = redirect.headers,
        .body = {},
        .timestamp = redirect.timestamp,
    };
}

[[nodiscard]] SerializableResponse makeMainDocumentResponse(const CapturedExchange &exchange)
{
    return SerializableResponse{
        .responseUrl = exchange.finalUrl,
        .method = "GET"_t,
        .pageId = exchange.pageId,
        .resourceType = "Document"_t,
        .statusCode = exchange.statusCode,
        .statusMessage = exchange.statusMessage,
        .headers = exchange.headers,
        .body = exchange.body,
        .timestamp = exchange.timestamp,
    };
}

[[nodiscard]] SerializableResponse makeResourceResponse(const CapturedResource &resource)
{
    return SerializableResponse{
        .responseUrl = resource.resourceUrl,
        .method = resource.method,
        .pageId = {},
        .resourceType = resource.resourceType,
        .statusCode = resource.statusCode,
        .statusMessage = resource.statusMessage,
        .headers = resource.headers,
        .body = resource.body,
        .timestamp = resource.timestamp,
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
    responses.reserve(
        numericCast<size_t>(
            ssize(exchange.mainDocumentRedirects) + ssize(exchange.resources) + 1_i64
        )
    );

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

    std::ranges::sort(responses, [](const auto &left, const auto &right) {
        return left.timestamp < right.timestamp;
    });
    return responses;
}

[[nodiscard]] std::pair<std::string, std::string>
serializeRecordPair(const SerializableResponse &response)
{
    const auto recordDate = response.timestamp;
    const auto responseRecordId = std::format(
        "urn:uuid:{}", us::utils::generators::GenerateBoostUuid()
    );
    const auto requestRecordId = std::format(
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
            ? String::fromBytes(
                  std::string(
                      http::StatusCodeString(numericCast<http::StatusCode>(response.statusCode))
                  )
              )
                  .expect()
            : response.statusMessage;

    std::string httpResponseHead = std::format(
        "HTTP/1.1 {} {}\r\n", response.statusCode, statusMessage
    );
    for (const auto &[name, value] : normalizedHeaders)
        httpResponseHead += std::format("{}: {}\r\n", name, value);
    httpResponseHead += "\r\n";

    std::string responseHeader = std::format(
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
        response.pageId.empty() ? "" : std::format("WARC-Page-ID: {}\r\n", response.pageId),
        response.resourceType ? std::format("WARC-Resource-Type: {}\r\n", *response.resourceType)
                              : std::string{},
        ssize(httpResponseHead) + ssize(response.body), httpResponseHead
    );

    std::string requestPayload = std::format(
        "{} {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: {}\r\n\r\n",
        response.method.empty() ? "GET"_t : response.method, requestPath, requestHost, kUserAgent
    );

    std::string requestHeader = std::format(
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
        response.pageId.empty() ? "" : std::format("WARC-Page-ID: {}\r\n", response.pageId),
        response.resourceType ? std::format("WARC-Resource-Type: {}\r\n", *response.resourceType)
                              : std::string{},
        ssize(requestPayload), requestPayload
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
            resource.resourceUrl, resource.statusCode, resource.headers, resource.resourceType
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
    const auto pageInfoUrl = std::format("urn:pageinfo:{}", pageUrl);
    const auto recordId = std::format("urn:uuid:{}", us::utils::generators::GenerateBoostUuid());
    const auto payload = buildPageInfoJson(exchange);

    return std::format(
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

    const auto defaultPortText = String::fromBytes(std::format("{}", defaultPort)).expect();
    return url.port() != defaultPortText;
}

[[nodiscard]] String cdxPayloadDigest(std::string_view payload)
{
    return String::fromBytes(sha256PrefixedHex(payload, "sha-256:")).expect();
}

[[nodiscard]] String cdxRecordDigest(std::string_view recordBytes)
{
    return String::fromBytes(sha256PrefixedHex(recordBytes, "sha256:")).expect();
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
    std::ranges::reverse(parts);

    std::string surtHost;
    for (size_t index = 0; index < parts.size(); index++) {
        if (index > 0)
            surtHost += ",";
        surtHost += parts[index];
    }

    auto path = std::string(maybeUrl->pathWithSearch().view());
    if (shouldIncludePort(*maybeUrl))
        surtHost += ":" + port;
    return String::fromBytes(surtHost + ")" + path).expect();
}

[[nodiscard]] std::string makeWaczIndexJson(const WarcCdxRecord &record)
{
    json::ValueBuilder recordEntry(json::Type::kObject);
    recordEntry["url"] = std::string(record.recordUrl.view());
    recordEntry["digest"] = std::string(record.digest.view());
    recordEntry["mime"] = contentTypeForHeaders(record.headers);
    recordEntry["filename"] = std::string(kWarcFilename);
    recordEntry["offset"] = raw(record.offset);
    recordEntry["length"] = raw(record.length);
    recordEntry["status"] = raw(record.statusCode);
    recordEntry["recordDigest"] = std::string(record.recordDigest.view());
    return json::ToString(recordEntry.ExtractValue());
}

[[nodiscard]] std::string buildCdx(const std::vector<WarcCdxRecord> &records)
{
    struct [[nodiscard]] CdxLine {
        std::string key;
        std::string timestamp;
        std::string json;
    };

    std::vector<CdxLine> lines;
    lines.reserve(records.size());
    for (const auto &record : records) {
        lines.push_back(
            CdxLine{
                .key = std::string(toSurtKey(record.recordUrl).view()),
                .timestamp = std::string(record.timestamp.view()),
                .json = makeWaczIndexJson(record),
            }
        );
    }
    std::ranges::sort(lines, [](const auto &left, const auto &right) {
        if (left.key != right.key)
            return left.key < right.key;
        return left.timestamp < right.timestamp;
    });

    std::string cdx;
    for (const auto &line : lines) {
        cdx += line.key;
        cdx += " ";
        cdx += line.timestamp;
        cdx += " ";
        cdx += line.json;
        cdx += "\n";
    }
    return cdx;
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
makeWaczResource(std::string_view name, std::string_view path, std::string_view body)
{
    dto::WaczResource item;
    item.name = std::string(name);
    item.path = std::string(path);
    item.hash = sha256PrefixedHex(body, "sha256:");
    item.bytes = numericCast<int64_t>(body.size());
    return item;
}

[[nodiscard]] std::vector<dto::WaczResource> buildWaczResources(
    std::string_view warcBytes, std::string_view pagesBytes, std::string_view cdxBytes
)
{
    std::vector<dto::WaczResource> resources;
    resources.reserve(3);
    resources.push_back(makeWaczResource("pages.jsonl", "pages/pages.jsonl", pagesBytes));
    resources.push_back(makeWaczResource(kWarcFilename, kWarcPath, warcBytes));
    resources.push_back(makeWaczResource("index.cdx", kIndexPath, cdxBytes));
    return resources;
}

[[nodiscard]] dto::WaczDataPackage
buildWaczDataPackage(const RunRequest &run, std::vector<dto::WaczResource> resources)
{
    dto::WaczDataPackage datapackage;
    datapackage.profile = "data-package";
    datapackage.created = datetime::TimePointTz(datetime::Now());
    datapackage.modified = datetime::TimePointTz(datetime::Now());
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
    entry.url = std::string(exchange.finalUrl.view());
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

Expected<std::string, ArtifactFailure> buildSuccessStdoutLog(
    const RunRequest &run, const CapturedExchange &exchange, i64 browserPid,
    ReusedBrowser reusedBrowser
)
{
    return std::format(
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
        exchange.redirectChain.empty() ? 0_i64 : ssize(exchange.redirectChain) - 1_i64, "chromium",
        browserPid, reusedBrowser == ReusedBrowser::kYes ? "true" : "false"
    );
}

Expected<WarcBuildOutput, ArtifactFailure> buildWarc(const CapturedExchange &exchange)
{
    const auto responses = collectSerializableResponses(exchange);

    WarcBuildOutput out;
    auto offset = 0_i64;
    for (const auto &response : responses) {
        auto [responseBytes, requestBytes] = serializeRecordPair(response);
        auto responseGz = gzipMember(responseBytes);
        if (!responseGz)
            return std::unexpected(responseGz.error());
        auto requestGz = gzipMember(requestBytes);
        if (!requestGz)
            return std::unexpected(requestGz.error());
        out.cdxRecords.push_back(
            WarcCdxRecord{
                .recordUrl = response.responseUrl,
                .timestamp = toCdxTimestamp(response.timestamp),
                .digest = cdxPayloadDigest(response.body),
                .recordDigest = cdxRecordDigest(*responseGz),
                .statusCode = response.statusCode,
                .headers = response.headers,
                .offset = offset,
                .length = i64(responseGz->size()),
            }
        );
        out.bytes.append(*responseGz);
        out.bytes.append(*requestGz);
        offset += i64{responseGz->size()} + i64{requestGz->size()};
    }

    const auto pageInfoUrl = text::format(
        "urn:pageinfo:{}", exchange.seedUrl.empty() ? exchange.finalUrl : exchange.seedUrl
    );
    auto pageInfoHeaders = std::unordered_map<std::string, std::string>{
        {"content-type", "application/json"},
    };
    const auto pageInfoBytes = serializePageInfoRecord(exchange);
    auto pageInfoGz = gzipMember(pageInfoBytes);
    if (!pageInfoGz)
        return std::unexpected(pageInfoGz.error());
    out.cdxRecords.push_back(
        WarcCdxRecord{
            .recordUrl = pageInfoUrl,
            .timestamp = toCdxTimestamp(pageTimestamp(exchange)),
            .digest = cdxPayloadDigest(buildPageInfoJson(exchange)),
            .recordDigest = cdxRecordDigest(*pageInfoGz),
            .statusCode = 200_i64,
            .headers = std::move(pageInfoHeaders),
            .offset = offset,
            .length = i64(pageInfoGz->size()),
        }
    );
    out.bytes.append(*pageInfoGz);
    return out;
}

Expected<std::string, ArtifactFailure> buildWacz(
    const RunRequest &run, const std::string &pagesJsonl, const WarcBuildOutput &warc,
    const std::string &stdoutLog, const std::string &stderrLog
)
{
    const auto cdx = buildCdx(warc.cdxRecords);
    const auto waczPages = buildWaczPages(pagesJsonl);
    auto resources = buildWaczResources(warc.bytes, waczPages, cdx);
    const auto datapackageJson = toJsonString(buildWaczDataPackage(run, std::move(resources)));

    arkhiv::ZipArchiveBuilder zip;
    arkhiv::ZipArchiveError error;
    const auto addFile = [&error, &zip](
                             std::string_view path, std::string_view body
                         ) -> Expected<void, ArtifactFailure> {
        if (!zip.addStoredFile(path, error, body))
            return std::unexpected(
                ArtifactFailure{.code = ArtifactError::kZipFailed, .detail = error.detail}
            );
        return {};
    };

    if (auto ok = addFile("datapackage.json", datapackageJson); !ok)
        return std::unexpected(ok.error());
    if (auto ok = addFile(kWarcPath, warc.bytes); !ok)
        return std::unexpected(ok.error());
    if (auto ok = addFile("pages/pages.jsonl", waczPages); !ok)
        return std::unexpected(ok.error());
    if (auto ok = addFile("logs/stdout.log", stdoutLog); !ok)
        return std::unexpected(ok.error());
    if (auto ok = addFile("logs/stderr.log", stderrLog); !ok)
        return std::unexpected(ok.error());
    if (auto ok = addFile(kIndexPath, cdx); !ok)
        return std::unexpected(ok.error());

    const auto zipBytes = zip.finish(error);
    if (!zipBytes)
        return std::unexpected(
            ArtifactFailure{.code = ArtifactError::kZipFailed, .detail = error.detail}
        );
    return *zipBytes;
}

} // namespace v1::crawler

namespace v1::crawler {

std::string computeContentSha256(const CapturedExchange &exchange)
{
    static constexpr std::string_view kItemDomain = "webshot.capture_hash.item";
    static constexpr std::string_view kCaptureDomain = "webshot.capture_hash";

    std::vector<std::string> itemDigests;
    itemDigests.reserve(
        numericCast<size_t>(
            ssize(exchange.mainDocumentRedirects) + ssize(exchange.resources) + 2_i64
        )
    );

    {
        std::string_view contentType;
        if (const auto it = exchange.headers.find("content-type"); it != std::end(exchange.headers))
            contentType = it->second;
        const auto urlDigest = sha256Bytes(exchange.finalUrl.view());
        const auto status = std::format("{}", exchange.statusCode);
        const auto statusDigest = sha256Bytes(status);
        const auto contentTypeDigest = sha256Bytes(contentType);
        const auto bodyDigest = sha256Bytes(exchange.body);
        itemDigests.emplace_back(sha256Bytes({
            kItemDomain,
            "main",
            urlDigest,
            statusDigest,
            contentTypeDigest,
            bodyDigest,
        }));
    }

    for (const auto &redirect : exchange.mainDocumentRedirects) {
        std::string_view location;
        if (const auto it = redirect.headers.find("location"); it != std::end(redirect.headers))
            location = it->second;
        const auto urlDigest = sha256Bytes(redirect.redirectUrl.view());
        const auto status = std::format("{}", redirect.statusCode);
        const auto statusDigest = sha256Bytes(status);
        const auto locationDigest = sha256Bytes(location);
        itemDigests.emplace_back(sha256Bytes({
            kItemDomain,
            "redirect",
            urlDigest,
            statusDigest,
            locationDigest,
        }));
    }

    for (const auto &resource : exchange.resources) {
        const auto method = resource.method.empty() ? "GET" : resource.method.view();
        const auto resourceType = resource.resourceType ? resource.resourceType->view() : "";
        std::string_view contentType;
        if (const auto it = resource.headers.find("content-type"); it != std::end(resource.headers))
            contentType = it->second;
        const auto urlDigest = sha256Bytes(resource.resourceUrl.view());
        const auto methodDigest = sha256Bytes(method);
        const auto status = std::format("{}", resource.statusCode);
        const auto statusDigest = sha256Bytes(status);
        const auto resourceTypeDigest = sha256Bytes(resourceType);
        const auto contentTypeDigest = sha256Bytes(contentType);
        const auto bodyDigest = sha256Bytes(resource.body);
        itemDigests.emplace_back(sha256Bytes({
            kItemDomain,
            "resource",
            urlDigest,
            methodDigest,
            statusDigest,
            resourceTypeDigest,
            contentTypeDigest,
            bodyDigest,
        }));
    }

    std::ranges::sort(itemDigests);

    std::string combined;
    combined.reserve(numericCast<size_t>(ssize(kCaptureDomain) + ssize(itemDigests) * 32_i64));
    combined.append(kCaptureDomain);
    for (const auto &d : itemDigests) {
        combined.append(d);
    }
    return sha256Bytes(combined);
}

} // namespace v1::crawler
