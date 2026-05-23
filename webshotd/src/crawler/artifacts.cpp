#include "crawler/artifacts.hpp"
#include "try.hpp"
#include "url.hpp"

#include <arkhiv/gzip.hpp>
#include <arkhiv/zip_archive.hpp>

#include "schema/browsertrix_pages.hpp"
#include "schema/wacz.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

#include <userver/crypto/hash.hpp>
#include <userver/formats/json.hpp>
#include <userver/http/status_code.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/datetime/from_string_saturating.hpp>
namespace ws::crawler {
namespace us = userver;
namespace json = us::formats::json;
namespace datetime = us::utils::datetime;
namespace http = us::http;
using namespace text::literals;

namespace {

constexpr std::string_view kUserAgent = "webshotd";
constexpr std::string_view kWarcPath = "archive/data.warc.gz";
constexpr std::string_view kWarcFilename = "data.warc.gz";
constexpr std::string_view kIndexPath = "indexes/index.cdx";

[[nodiscard]] std::string Sha256Bytes(std::string_view data)
{
    return us::crypto::hash::Sha256(data, us::crypto::hash::OutputEncoding::kBinary);
}

[[nodiscard]] std::string Sha256Bytes(std::initializer_list<std::string_view> data)
{
    return us::crypto::hash::Sha256(data, us::crypto::hash::OutputEncoding::kBinary);
}

[[nodiscard]] std::string Sha256Hex(std::string_view data)
{
    return us::crypto::hash::Sha256(data, us::crypto::hash::OutputEncoding::kHex);
}

[[nodiscard]] std::string Sha256PrefixedHex(std::string_view data, std::string_view prefix)
{
    return std::format("{}{}", prefix, Sha256Hex(data));
}

[[nodiscard]] Expected<std::string, ArtifactError> GzipMember(std::string_view body) noexcept
{
    arkhiv::GzipError error;
    auto maybe_bytes = arkhiv::GzipCompressMember(body, error);
    if (!maybe_bytes)
        return Unex(ArtifactError{.code = ArtifactErrorCode::kGzipFailed, .detail = error.detail});
    return std::move(*maybe_bytes);
}

[[nodiscard]] std::string ExtractHtmlTitle(std::string_view body)
{
    auto open_pos = body.find("<title");
    if (open_pos == std::string_view::npos)
        return {};
    auto start = body.find('>', open_pos);
    if (start == std::string_view::npos)
        return {};
    auto end = body.find("</title>", start + 1);
    if (end == std::string_view::npos)
        return {};
    return std::string{body.substr(start + 1, end - start - 1)};
}

template <typename T> [[nodiscard]] std::string ToJsonBytes(const T &value)
{
    return json::ToString(json::ValueBuilder(value).ExtractValue());
}

[[nodiscard]] String ToCdxTimestamp(const String &iso)
{
    return *String::FromBytes(
        datetime::UtcTimestring(
            datetime::FromRfc3339StringSaturating(iso.ToBytes()), "%Y%m%d%H%M%S"
        )
    );
}

[[nodiscard]] std::unordered_map<std::string, std::string>
NormalizeResponseHeaders(const std::unordered_map<std::string, std::string> &headers)
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
    String response_url;
    String method;
    String page_id;
    std::optional<String> resource_type;
    i64 status_code{0};
    String status_message;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    String timestamp;
};

[[nodiscard]] SerializableResponse
MakeRedirectResponse(const CapturedMainDocumentRedirect &redirect)
{
    return {
        .response_url = redirect.redirect_url,
        .method = "GET"_t,
        .page_id = {},
        .resource_type = "Document"_t,
        .status_code = redirect.status_code,
        .status_message = redirect.status_message,
        .headers = redirect.headers,
        .body = {},
        .timestamp = redirect.timestamp,
    };
}

[[nodiscard]] SerializableResponse MakeMainDocumentResponse(const CapturedExchange &exchange)
{
    return {
        .response_url = exchange.final_url,
        .method = "GET"_t,
        .page_id = exchange.page_id,
        .resource_type = "Document"_t,
        .status_code = exchange.status_code,
        .status_message = exchange.status_message,
        .headers = exchange.headers,
        .body = exchange.body,
        .timestamp = exchange.timestamp,
    };
}

[[nodiscard]] SerializableResponse MakeResourceResponse(const CapturedResource &resource)
{
    return {
        .response_url = resource.resource_url,
        .method = resource.method,
        .page_id = {},
        .resource_type = resource.resource_type,
        .status_code = resource.status_code,
        .status_message = resource.status_message,
        .headers = resource.headers,
        .body = resource.body,
        .timestamp = resource.timestamp,
    };
}

[[nodiscard]] std::string
ContentTypeForHeaders(const std::unordered_map<std::string, std::string> &headers)
{
    if (auto it = headers.find("content-type"); it != std::end(headers))
        return it->second;
    return "application/octet-stream";
}

[[nodiscard]] String PageTimestamp(const CapturedExchange &exchange)
{
    if (!exchange.main_document_redirects.empty())
        return exchange.main_document_redirects.front().timestamp;
    return exchange.timestamp;
}

[[nodiscard]] std::vector<SerializableResponse>
CollectSerializableResponses(const CapturedExchange &exchange)
{
    std::vector<SerializableResponse> responses;
    responses.reserve(
        NumericCast<size_t>(
            ssize(exchange.main_document_redirects) + ssize(exchange.resources) + 1_i64
        )
    );

    for (const auto &redirect : exchange.main_document_redirects) {
        auto response = MakeRedirectResponse(redirect);
        response.page_id = exchange.page_id;
        responses.push_back(std::move(response));
    }
    responses.push_back(MakeMainDocumentResponse(exchange));
    for (const auto &resource : exchange.resources) {
        auto response = MakeResourceResponse(resource);
        response.page_id = exchange.page_id;
        responses.push_back(std::move(response));
    }

    std::ranges::sort(responses, [](const auto &left, const auto &right) {
        return left.timestamp < right.timestamp;
    });
    return responses;
}

[[nodiscard]] std::pair<std::string, std::string>
SerializeRecordPair(const SerializableResponse &response)
{
    const auto record_date = response.timestamp;
    auto response_record_id = std::format(
        "urn:uuid:{}", us::utils::generators::GenerateBoostUuid()
    );
    const auto request_record_id = std::format(
        "urn:uuid:{}", us::utils::generators::GenerateBoostUuid()
    );
    const auto normalized_headers = NormalizeResponseHeaders(response.headers);
    auto request_path = "/"_t;
    String request_host;
    if (const auto url_text = String::FromBytes(response.response_url.View())) {
        if (auto maybe_url = Url::FromText(*url_text)) {
            request_path = maybe_url->PathWithSearch();
            request_host = maybe_url->Host();
        }
    }
    const auto status_message =
        response.status_message.Empty()
            ? *String::FromBytes(
                  std::string(
                      http::StatusCodeString(NumericCast<http::StatusCode>(response.status_code))
                  )
              )
            : response.status_message;

    std::string http_response_head = std::format(
        "HTTP/1.1 {} {}\r\n", response.status_code, status_message
    );
    for (const auto &[name, value] : normalized_headers)
        http_response_head += std::format("{}: {}\r\n", name, value);
    http_response_head += "\r\n";

    std::string response_header = std::format(
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
        response.response_url, record_date, response_record_id,
        response.page_id.Empty() ? "" : std::format("WARC-Page-ID: {}\r\n", response.page_id),
        response.resource_type ? std::format("WARC-Resource-Type: {}\r\n", *response.resource_type)
                               : std::string{},
        ssize(http_response_head) + ssize(response.body), http_response_head
    );

    std::string request_payload = std::format(
        "{} {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: {}\r\n\r\n",
        response.method.Empty() ? "GET"_t : response.method, request_path, request_host, kUserAgent
    );

    std::string request_header = std::format(
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
        response.response_url, record_date, request_record_id, response_record_id,
        response.page_id.Empty() ? "" : std::format("WARC-Page-ID: {}\r\n", response.page_id),
        response.resource_type ? std::format("WARC-Resource-Type: {}\r\n", *response.resource_type)
                               : std::string{},
        ssize(request_payload), request_payload
    );

    response_header += response.body;
    response_header += "\r\n\r\n";
    return {std::move(response_header), std::move(request_header)};
}

[[nodiscard]] std::string MakePageInfoJsonBytes(const CapturedExchange &exchange)
{
    json::ValueBuilder page_info(json::Type::kObject);
    page_info["pageid"] = exchange.page_id.ToBytes();
    page_info["url"] = std::string(
        (exchange.seed_url.Empty() ? exchange.final_url : exchange.seed_url).View()
    );
    page_info["ts"] = PageTimestamp(exchange).ToBytes();

    json::ValueBuilder urls(json::Type::kObject);
    const auto append_url = [&urls](
                                const String &url, i64 status_code,
                                const std::unordered_map<std::string, std::string> &headers,
                                std::optional<String> resource_type
                            ) {
        json::ValueBuilder value(json::Type::kObject);
        value["status"] = Raw(status_code);
        auto mime = ContentTypeForHeaders(headers);
        if (!mime.empty())
            value["mime"] = mime;
        if (resource_type && !resource_type->Empty())
            value["type"] = resource_type->ToBytes();
        urls[url.ToBytes()] = value.ExtractValue();
    };

    for (const auto &redirect : exchange.main_document_redirects)
        append_url(redirect.redirect_url, redirect.status_code, redirect.headers, "Document"_t);
    append_url(exchange.final_url, exchange.status_code, exchange.headers, "Document"_t);
    for (const auto &resource : exchange.resources) {
        append_url(
            resource.resource_url, resource.status_code, resource.headers, resource.resource_type
        );
    }
    page_info["urls"] = urls.ExtractValue();

    json::ValueBuilder counts(json::Type::kObject);
    counts["jsErrors"] = 0;
    page_info["counts"] = counts.ExtractValue();
    return json::ToString(page_info.ExtractValue());
}

[[nodiscard]] std::string SerializePageInfoRecord(const CapturedExchange &exchange)
{
    auto page_url = exchange.seed_url.Empty() ? exchange.final_url : exchange.seed_url;
    auto page_info_url = std::format("urn:pageinfo:{}", page_url);
    auto record_id = std::format("urn:uuid:{}", us::utils::generators::GenerateBoostUuid());
    auto payload = MakePageInfoJsonBytes(exchange);

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
        page_info_url, PageTimestamp(exchange), record_id, exchange.page_id, payload.size(), payload
    );
}

[[nodiscard]] String CdxPayloadDigest(std::string_view payload)
{
    return *String::FromBytes(Sha256PrefixedHex(payload, "sha-256:"));
}

[[nodiscard]] String CdxRecordDigest(std::string_view record_bytes)
{
    return *String::FromBytes(Sha256PrefixedHex(record_bytes, "sha256:"));
}

[[nodiscard]] String ToSurtKey(const String &url_text)
{
    auto maybe_url = Url::FromText(url_text);
    if (!maybe_url)
        return url_text;

    if (!maybe_url->IsHttpOrHttps())
        return url_text;

    return maybe_url->Surt();
}

[[nodiscard]] std::string MakeWaczIndexJsonBytes(const WarcCdxRecord &record)
{
    json::ValueBuilder record_entry(json::Type::kObject);
    record_entry["url"] = record.record_url.ToBytes();
    record_entry["digest"] = record.digest.ToBytes();
    record_entry["mime"] = ContentTypeForHeaders(record.headers);
    record_entry["filename"] = std::string(kWarcFilename);
    record_entry["offset"] = Raw(record.offset);
    record_entry["length"] = Raw(record.length);
    record_entry["status"] = Raw(record.status_code);
    record_entry["recordDigest"] = record.record_digest.ToBytes();
    return json::ToString(record_entry.ExtractValue());
}

[[nodiscard]] std::string MakeCdx(const std::vector<WarcCdxRecord> &records)
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
                .key = ToSurtKey(record.record_url).ToBytes(),
                .timestamp = record.timestamp.ToBytes(),
                .json = MakeWaczIndexJsonBytes(record),
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

[[nodiscard]] std::string MakeWaczPagesBytes(std::string_view pages_jsonl)
{
    json::ValueBuilder pages_header(json::Type::kObject);
    pages_header["format"] = "json-pages-1.0";
    pages_header["id"] = "pages";
    pages_header["title"] = "All Pages";
    pages_header["hasText"] = false;
    return json::ToString(pages_header.ExtractValue()) + "\n" + std::string(pages_jsonl);
}

[[nodiscard]] dto::WaczResource
MakeWaczResource(std::string_view name, std::string_view path, std::string_view body)
{
    return {
        .name = std::string(name),
        .path = std::string(path),
        .hash = Sha256PrefixedHex(body, "sha256:"),
        .bytes = NumericCast<int64_t>(body.size()),
    };
}

[[nodiscard]] std::vector<dto::WaczResource> MakeWaczResources(
    std::string_view warc_bytes, std::string_view pages_bytes, std::string_view cdx_bytes
)
{
    std::vector<dto::WaczResource> resources;
    resources.reserve(3);
    resources.push_back(MakeWaczResource("pages.jsonl", "pages/pages.jsonl", pages_bytes));
    resources.push_back(MakeWaczResource(kWarcFilename, kWarcPath, warc_bytes));
    resources.push_back(MakeWaczResource("index.cdx", kIndexPath, cdx_bytes));
    return resources;
}

[[nodiscard]] dto::WaczDataPackage
MakeWaczDataPackage(const RunRequest &run, std::vector<dto::WaczResource> resources)
{
    auto created = datetime::TimePointTz(datetime::Now());
    return {
        .profile = "data-package",
        .resources = std::move(resources),
        .wacz_version = "1.1.1",
        .title = run.seed_url.ToBytes(),
        .software = "webshotd",
        .created = created,
        .modified = created,
    };
}

} // namespace

std::string MakePagesJsonl(const CapturedExchange &exchange)
{
    dto::BrowsertrixPageEntry entry{
        .id = exchange.page_id.ToBytes(),
        .url = exchange.final_url.ToBytes(),
        .title = exchange.title ? exchange.title->ToBytes() : ExtractHtmlTitle(exchange.body),
        .loadState = exchange.status_code >= 200_i64 && exchange.status_code < 400_i64 ? 2 : 0,
        .mime = ContentTypeForHeaders(exchange.headers),
        .seed = true,
    };
    entry.ts = datetime::TimePointTz(
        datetime::FromRfc3339StringSaturating(PageTimestamp(exchange).ToBytes())
    );
    entry.status = Raw(exchange.status_code);
    entry.depth = 0;
    return ToJsonBytes(entry) + "\n";
}

Expected<std::string, ArtifactError> MakeSuccessStdoutLog(
    const RunRequest &run, const CapturedExchange &exchange, i64 browser_pid,
    ReusedBrowser reused_browser
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
        run.seed_url, exchange.final_url, exchange.status_code,
        exchange.redirect_chain.empty() ? 0_i64 : ssize(exchange.redirect_chain) - 1_i64,
        "chromium", browser_pid, reused_browser == ReusedBrowser::kYes ? "true" : "false"
    );
}

Expected<WarcBuildOutput, ArtifactError> MakeWarc(const CapturedExchange &exchange)
{
    const auto responses = CollectSerializableResponses(exchange);

    WarcBuildOutput out;
    i64 offset{0};
    for (const auto &response : responses) {
        auto [response_bytes, request_bytes] = SerializeRecordPair(response);
        auto response_gz = TRY(GzipMember(response_bytes));
        auto request_gz = TRY(GzipMember(request_bytes));
        out.cdx_records.push_back(
            WarcCdxRecord{
                .record_url = response.response_url,
                .timestamp = ToCdxTimestamp(response.timestamp),
                .digest = CdxPayloadDigest(response.body),
                .record_digest = CdxRecordDigest(response_gz),
                .status_code = response.status_code,
                .headers = response.headers,
                .offset = offset,
                .length = ssize(response_gz),
            }
        );
        out.bytes.append(response_gz);
        out.bytes.append(request_gz);
        offset += i64{response_gz.size()} + i64{request_gz.size()};
    }

    const auto page_info_url = text::Format(
        "urn:pageinfo:{}", exchange.seed_url.Empty() ? exchange.final_url : exchange.seed_url
    );
    std::unordered_map<std::string, std::string> page_info_headers{
        {"content-type", "application/json"},
    };
    const auto page_info_bytes = SerializePageInfoRecord(exchange);
    auto page_info_gz = TRY(GzipMember(page_info_bytes));
    out.cdx_records.push_back(
        WarcCdxRecord{
            .record_url = page_info_url,
            .timestamp = ToCdxTimestamp(PageTimestamp(exchange)),
            .digest = CdxPayloadDigest(MakePageInfoJsonBytes(exchange)),
            .record_digest = CdxRecordDigest(page_info_gz),
            .status_code = 200_i64,
            .headers = std::move(page_info_headers),
            .offset = offset,
            .length = ssize(page_info_gz),
        }
    );
    out.bytes.append(page_info_gz);
    return out;
}

Expected<std::string, ArtifactError> MakeWacz(
    const RunRequest &run, const std::string &pages_jsonl, const WarcBuildOutput &warc,
    const std::string &stdout_log, const std::string &stderr_log
)
{
    auto cdx = MakeCdx(warc.cdx_records);
    auto wacz_pages = MakeWaczPagesBytes(pages_jsonl);
    auto resources = MakeWaczResources(warc.bytes, wacz_pages, cdx);
    auto datapackage_json = ToJsonBytes(MakeWaczDataPackage(run, std::move(resources)));

    arkhiv::ZipArchiveBuilder zip;
    arkhiv::ZipArchiveError error;
    auto add_file = [&error, &zip](
                        std::string_view path, std::string_view body
                    ) -> Expected<void, ArtifactError> {
        if (!zip.AddStoredFile(path, error, body))
            return Unex(
                ArtifactError{.code = ArtifactErrorCode::kZipFailed, .detail = error.detail}
            );
        return {};
    };

    TRY(add_file("datapackage.json", datapackage_json));
    TRY(add_file(kWarcPath, warc.bytes));
    TRY(add_file("pages/pages.jsonl", wacz_pages));
    TRY(add_file("logs/stdout.log", stdout_log));
    TRY(add_file("logs/stderr.log", stderr_log));
    TRY(add_file(kIndexPath, cdx));

    auto zip_bytes = zip.Finish(error);
    if (!zip_bytes)
        return Unex(ArtifactError{.code = ArtifactErrorCode::kZipFailed, .detail = error.detail});
    return *zip_bytes;
}

} // namespace ws::crawler

namespace ws::crawler {

std::string ComputeContentSha256(const CapturedExchange &exchange)
{
    static constexpr std::string_view item_domain = "webshot.capture_hash.item";
    static constexpr std::string_view capture_domain = "webshot.capture_hash";

    std::vector<std::string> item_digests;
    item_digests.reserve(
        NumericCast<size_t>(
            ssize(exchange.main_document_redirects) + ssize(exchange.resources) + 2_i64
        )
    );

    {
        std::string_view content_type;
        if (auto it = exchange.headers.find("content-type"); it != std::end(exchange.headers))
            content_type = it->second;
        auto url_digest = Sha256Bytes(exchange.final_url.View());
        auto status = std::format("{}", exchange.status_code);
        auto status_digest = Sha256Bytes(status);
        auto content_type_digest = Sha256Bytes(content_type);
        auto body_digest = Sha256Bytes(exchange.body);
        item_digests.emplace_back(Sha256Bytes({
            item_domain,
            "main",
            url_digest,
            status_digest,
            content_type_digest,
            body_digest,
        }));
    }

    for (const auto &redirect : exchange.main_document_redirects) {
        std::string_view location;
        if (auto it = redirect.headers.find("location"); it != std::end(redirect.headers))
            location = it->second;
        auto url_digest = Sha256Bytes(redirect.redirect_url.View());
        auto status = std::format("{}", redirect.status_code);
        auto status_digest = Sha256Bytes(status);
        auto location_digest = Sha256Bytes(location);
        item_digests.emplace_back(Sha256Bytes({
            item_domain,
            "redirect",
            url_digest,
            status_digest,
            location_digest,
        }));
    }

    for (const auto &resource : exchange.resources) {
        auto method = resource.method.Empty() ? "GET" : resource.method.View();
        auto resource_type = resource.resource_type ? resource.resource_type->View() : "";
        std::string_view content_type;
        if (auto it = resource.headers.find("content-type"); it != std::end(resource.headers))
            content_type = it->second;
        auto url_digest = Sha256Bytes(resource.resource_url.View());
        auto method_digest = Sha256Bytes(method);
        auto status = std::format("{}", resource.status_code);
        auto status_digest = Sha256Bytes(status);
        auto resource_type_digest = Sha256Bytes(resource_type);
        auto content_type_digest = Sha256Bytes(content_type);
        auto body_digest = Sha256Bytes(resource.body);
        item_digests.emplace_back(Sha256Bytes({
            item_domain,
            "resource",
            url_digest,
            method_digest,
            status_digest,
            resource_type_digest,
            content_type_digest,
            body_digest,
        }));
    }

    std::ranges::sort(item_digests);

    std::string combined;
    combined.reserve(NumericCast<size_t>(ssize(capture_domain) + ssize(item_digests) * 32_i64));
    combined.append(capture_domain);
    for (const auto &d : item_digests) {
        combined.append(d);
    }
    return Sha256Bytes(combined);
}

} // namespace ws::crawler
