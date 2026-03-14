#include "crawler/artifacts.hpp"
#include "ip_utils.hpp"
#include "url.hpp"

#include "schema/browsertrix_pages.hpp"
#include "schema/wacz.hpp"

#include <algorithm>
#include <array>
#include <boost/uuid/uuid_io.hpp>
#include <initializer_list>
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

[[nodiscard]] u32 computeCrc32(std::string_view bytes)
{
    static const auto table = [] {
        std::array<u32, 256> values{};
        for (size_t index = 0; index < values.size(); index++) {
            auto value = u32(index);
            for (size_t bit = 0; bit < 8; bit++) {
                value = (value & 1_u32) == 1_u32 ? 0xedb88320_u32 ^ (value >> 1_u32)
                                                 : value >> 1_u32;
            }
            values[index] = value;
        }
        return values;
    }();

    auto crc = 0xffffffff_u32;
    for (const auto ch : bytes) {
        const auto byte = u32(static_cast<uint8_t>(ch));
        crc = (crc >> 8_u32) ^ table[toNative((crc ^ byte) & 0xff_u32)];
    }
    return crc ^ 0xffffffff_u32;
}

void appendU16Le(std::string &out, uint16_t value)
{
    for (size_t index = 0; index < sizeof(value); index++) {
        out.push_back(static_cast<char>((static_cast<uint64_t>(value) >> (index * 8U)) & 0xffU));
    }
}

void appendU32Le(std::string &out, uint32_t value)
{
    for (size_t index = 0; index < sizeof(value); index++) {
        out.push_back(static_cast<char>((static_cast<uint64_t>(value) >> (index * 8U)) & 0xffU));
    }
}

void appendU16Fields(std::string &out, std::initializer_list<uint16_t> values)
{
    for (const auto value : values)
        appendU16Le(out, value);
}

void appendU32Fields(std::string &out, std::initializer_list<uint32_t> values)
{
    for (const auto value : values)
        appendU32Le(out, value);
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

class [[nodiscard]] StoredZipBuilder final {
public:
    void addFile(std::string_view name, std::string_view body)
    {
        const auto crc32 = computeCrc32(body);
        const auto localHeaderOffset = numericCast<uint32_t>(archiveBytes.size());
        appendLocalFile(name, body, crc32);
        appendCentralDirectoryEntry(name, body, crc32, localHeaderOffset);
        entryCount++;
    }

    [[nodiscard]] std::string finish() &&
    {
        const auto centralDirectoryOffset = numericCast<uint32_t>(archiveBytes.size());
        archiveBytes += centralDirectoryBytes;

        appendU32Fields(archiveBytes, {kEndOfCentralDirectorySignature});
        appendU16Fields(
            archiveBytes, {
                              0U,
                              0U,
                              numericCast<uint16_t>(entryCount),
                              numericCast<uint16_t>(entryCount),
                          }
        );
        appendU32Fields(
            archiveBytes, {
                              numericCast<uint32_t>(centralDirectoryBytes.size()),
                              centralDirectoryOffset,
                          }
        );
        appendU16Fields(archiveBytes, {0U});
        return std::move(archiveBytes);
    }

private:
    static constexpr uint32_t kLocalFileHeaderSignature = 0x04034b50U;
    static constexpr uint32_t kCentralDirectoryHeaderSignature = 0x02014b50U;
    static constexpr uint32_t kEndOfCentralDirectorySignature = 0x06054b50U;
    static constexpr uint16_t kVersion20 = 20U;

    void appendLocalFile(std::string_view name, std::string_view body, u32 crc32)
    {
        appendU32Fields(archiveBytes, {kLocalFileHeaderSignature});
        appendU16Fields(archiveBytes, {kVersion20, 0U, 0U, 0U, 0U});
        appendU32Fields(
            archiveBytes, {
                              toNative(crc32),
                              numericCast<uint32_t>(body.size()),
                              numericCast<uint32_t>(body.size()),
                          }
        );
        appendU16Fields(archiveBytes, {numericCast<uint16_t>(name.size()), 0U});
        archiveBytes += name;
        archiveBytes += body;
    }

    void appendCentralDirectoryEntry(
        std::string_view name, std::string_view body, u32 crc32, uint32_t localHeaderOffset
    )
    {
        appendU32Fields(centralDirectoryBytes, {kCentralDirectoryHeaderSignature});
        appendU16Fields(
            centralDirectoryBytes, {
                                       kVersion20,
                                       kVersion20,
                                       0U,
                                       0U,
                                       0U,
                                       0U,
                                   }
        );
        appendU32Fields(
            centralDirectoryBytes, {
                                       toNative(crc32),
                                       numericCast<uint32_t>(body.size()),
                                       numericCast<uint32_t>(body.size()),
                                   }
        );
        appendU16Fields(
            centralDirectoryBytes, {
                                       numericCast<uint16_t>(name.size()),
                                       0U,
                                       0U,
                                       0U,
                                       0U,
                                   }
        );
        appendU32Fields(centralDirectoryBytes, {0U, localHeaderOffset});
        centralDirectoryBytes += name;
    }

    std::string archiveBytes;
    std::string centralDirectoryBytes;
    size_t entryCount{0};
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
    struct ZipFileSpec {
        std::string_view path;
        std::string_view body;
    };

    const auto cdxj = buildCdxj(warc.cdxRecords);
    const auto waczPages = buildWaczPages(pagesJsonl);
    auto resources = buildWaczResources(
        warc.bytes.size(), waczPages.size(), stdoutLog.size(), stderrLog.size(), cdxj.size()
    );
    const auto datapackageJson = toJsonString(buildWaczDataPackage(run, std::move(resources)));
    const auto files = std::array<ZipFileSpec, 6>{{
        {"datapackage.json", datapackageJson},
        {"archive/data.warc", warc.bytes},
        {"pages/pages.jsonl", waczPages},
        {"logs/stdout.log", stdoutLog},
        {"logs/stderr.log", stderrLog},
        {"indexes/index.cdxj", cdxj},
    }};

    StoredZipBuilder zip;
    for (const auto &file : files)
        zip.addFile(file.path, file.body);
    return std::move(zip).finish();
}

} // namespace v1::crawler
