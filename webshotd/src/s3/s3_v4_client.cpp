/**
 * @file
 * @brief Minimal S3 client that signs requests with AWS Signature V4.
 *
 * Implements the subset of methods required by this service (PUT, HEAD, DELETE)
 * and presign helpers. It relies on userver HTTP client for transport.
 */
#include "s3/s3_v4_client.hpp"
#include "s3/sigv4_signer.hpp"

#include "link.hpp"
#include "text.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include <absl/strings/match.h>

#include <userver/crypto/hash.hpp>

#include <userver/clients/http/client.hpp>
#include <userver/clients/http/response.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime.hpp>

namespace s3 = userver::s3api;
namespace http = userver::clients::http;
using namespace text::literals;

namespace v1::s3v4 {

namespace detail {

EndpointParts parseEndpoint(const String &ep)
{
    const auto link = Link::fromText(ep, ep.sizeBytes());
    const auto &url = link.url;

    UINVARIANT(url.isHttp() || url.isHttps(), "S3 endpoint must be http or https");

    UINVARIANT(!url.hasSearch(), "S3 endpoint must not include query");
    std::string path = std::string(url.pathname().view());
    if (path.empty())
        path = "/";
    UINVARIANT(path == "/", "S3 endpoint path must be root");

    return {url, url.host(), url.hostname(), url.hasPort() ? url.port() : String{}, "/"_t};
}
} // namespace detail

S3V4Client::S3V4Client(
    http::Client &http, S3V4Config cfg, S3Credentials creds, String defaultBucket
)
    : httpClient(http), config(std::move(cfg)), creds(std::move(creds)),
      bucketName(std::move(defaultBucket)), endpoint(detail::parseEndpoint(config.endpoint))
{
}

// methods used by this service
std::string S3V4Client::PutObject(
    std::string_view path, std::string data, const std::optional<Meta> &meta,
    std::string_view contentType, const std::optional<std::string> &contentDisposition,
    const std::optional<std::vector<Tag>> &tags
) const
{
    static_cast<void>(tags);
    const auto pathText = String::fromBytesThrow(std::string(path));
    const auto built = makePathStyleUrl(pathText, {});
    http::Headers headers;
    headers[userver::http::headers::kContentType] = std::string(contentType);
    if (contentDisposition)
        headers[userver::http::headers::kContentDisposition] = contentDisposition.value();
    if (meta) {
        for (const auto &kv : meta.value())
            headers["x-amz-meta-" + kv.first] = kv.second;
    }
    const String payloadHash = sha256Hex(data);
    signRequest("PUT"_t, built.rawPath, built.host, headers, payloadHash);

    const std::string url = std::string(built.href.view());
    auto resp = httpClient.CreateNotSignedRequest()
                    .put(url, std::move(data))
                    .headers(headers)
                    .timeout(config.timeout)
                    .perform();
    resp->raise_for_status();
    return resp->body();
}

void S3V4Client::DeleteObject(std::string_view path) const
{
    const auto pathText = String::fromBytesThrow(std::string(path));
    const auto built = makePathStyleUrl(pathText, {});
    http::Headers headers;
    const auto payloadHash = sha256Hex("");
    signRequest("DELETE"_t, built.rawPath, built.host, headers, payloadHash);

    const std::string url = std::string(built.href.view());
    auto resp = httpClient.CreateNotSignedRequest()
                    .delete_method(url)
                    .headers(headers)
                    .timeout(config.timeout)
                    .perform();
    resp->raise_for_status();
}

// Minimal header HEAD support (used indirectly by copy in userver, not used here)
std::optional<s3::Client::HeadersDataResponse>
S3V4Client::GetObjectHead(std::string_view path, const HeaderDataRequest &request) const
{
    const auto pathText = String::fromBytesThrow(std::string(path));
    const auto built = makePathStyleUrl(pathText, {});
    http::Headers headers;
    const auto payloadHash = sha256Hex("");
    signRequest("HEAD"_t, built.rawPath, built.host, headers, payloadHash);
    const auto url = std::string(built.href.view());
    auto resp = httpClient.CreateNotSignedRequest()
                    .head(url)
                    .headers(headers)
                    .timeout(config.timeout)
                    .perform();
    resp->raise_for_status();
    HeadersDataResponse out;
    if (request.need_meta) {
        static constexpr std::string_view kMetaPrefix = "x-amz-meta-";
        out.meta.emplace();
        for (const auto &kv : resp->headers()) {
            const auto &name = kv.first;
            if (absl::StartsWithIgnoreCase(std::string_view{name}, kMetaPrefix)) {
                out.meta->emplace(name.substr(kMetaPrefix.size()), kv.second);
            }
        }
    }
    if (request.headers) {
        out.headers.emplace();
        for (const auto &wanted : request.headers.value()) {
            auto it = resp->headers().find(wanted);
            if (it != resp->headers().end())
                out.headers->emplace(it->first, it->second);
        }
    }
    return out;
}

// Unused by this service; provide stubs that throw to make it explicit
std::optional<std::string> S3V4Client::GetObject(
    std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &
) const
{
    throw std::runtime_error("GetObject not implemented in SigV4 client for this service");
}

std::string S3V4Client::TryGetObject(
    std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &
) const
{
    throw std::runtime_error("TryGetObject not implemented in SigV4 client");
}

std::optional<std::string> S3V4Client::GetPartialObject(
    std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *,
    const HeaderDataRequest &
) const
{
    throw std::runtime_error("GetPartialObject not implemented in SigV4 client");
}

std::string S3V4Client::TryGetPartialObject(
    std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *,
    const HeaderDataRequest &
) const
{
    throw std::runtime_error("TryGetPartialObject not implemented in SigV4 client");
}

std::string S3V4Client::CopyObject(
    std::string_view, std::string_view, std::string_view, const std::optional<Meta> &
)
{
    throw std::runtime_error("CopyObject not implemented in SigV4 client");
}

std::string S3V4Client::CopyObject(std::string_view, std::string_view, const std::optional<Meta> &)
{
    throw std::runtime_error("CopyObject not implemented in SigV4 client");
}

std::optional<std::string>
S3V4Client::ListBucketContents(std::string_view, int, std::string, std::string) const
{
    throw std::runtime_error("ListBucketContents not implemented in SigV4 client");
}

std::vector<s3::ObjectMeta> S3V4Client::ListBucketContentsParsed(std::string_view) const
{
    throw std::runtime_error("ListBucketContentsParsed not implemented in SigV4 client");
}

std::vector<std::string> S3V4Client::ListBucketDirectories(std::string_view) const
{
    throw std::runtime_error("ListBucketDirectories not implemented in SigV4 client");
}

void S3V4Client::UpdateConfig(s3::ConnectionCfg &&) {}

std::string_view S3V4Client::GetBucketName() const { return bucketName.view(); }

// Multipart upload API stubs (not used by this service)
userver::s3api::multipart_upload::InitiateMultipartUploadResult S3V4Client::CreateMultipartUpload(
    const userver::s3api::multipart_upload::CreateMultipartUploadRequest &
) const
{
    throw std::runtime_error("CreateMultipartUpload not implemented in SigV4 client");
}

userver::s3api::multipart_upload::UploadPartResult
S3V4Client::UploadPart(const userver::s3api::multipart_upload::UploadPartRequest &) const
{
    throw std::runtime_error("UploadPart not implemented in SigV4 client");
}

userver::s3api::multipart_upload::CompleteMultipartUploadResult S3V4Client::CompleteMultipartUpload(
    const userver::s3api::multipart_upload::CompleteMultipartUploadRequest &
) const
{
    throw std::runtime_error("CompleteMultipartUpload not implemented in SigV4 client");
}

void S3V4Client::AbortMultipartUpload(
    const userver::s3api::multipart_upload::AbortMultipartUploadRequest &
) const
{
    throw std::runtime_error("AbortMultipartUpload not implemented in SigV4 client");
}

userver::s3api::multipart_upload::ListPartsResult
S3V4Client::ListParts(const userver::s3api::multipart_upload::ListPartsRequest &) const
{
    throw std::runtime_error("ListParts not implemented in SigV4 client");
}

userver::s3api::multipart_upload::ListMultipartUploadsResult S3V4Client::ListMultipartUploads(
    const userver::s3api::multipart_upload::ListMultipartUploadsRequest &
) const
{
    throw std::runtime_error("ListMultipartUploads not implemented in SigV4 client");
}

// Presign support using chrono
std::string
S3V4Client::GenerateDownloadUrl(std::string_view path, time_t expiresEpoch, bool useSsl) const
{
    const auto expiresAt = std::chrono::system_clock::from_time_t(expiresEpoch);
    const auto method = "GET"_t;
    const auto pathText = String::fromBytesThrow(path);
    const auto protocolText = useSsl ? "https"_t : "http"_t;
    const auto urlText = presignPathStyle(method, pathText, expiresAt, protocolText);
    return std::string(urlText.view());
}

std::string S3V4Client::GenerateDownloadUrlVirtualHostAddressing(
    std::string_view path, const std::chrono::system_clock::time_point &expiresAt,
    std::string_view protocol
) const
{
    if (bucketName.empty())
        throw std::runtime_error("presign requires non-empty bucket");
    auto method = "GET"_t;
    auto pathText = String::fromBytesThrow(path);
    auto protocolText = String::fromBytesThrow(protocol);
    return std::string(presignVirtualHost(method, pathText, expiresAt, protocolText, {}).view());
}

std::string S3V4Client::GenerateUploadUrlVirtualHostAddressing(
    std::string_view data, std::string_view contentType, std::string_view path,
    const std::chrono::system_clock::time_point &expiresAt, std::string_view protocol
) const
{
    if (bucketName.empty())
        throw std::runtime_error("presign requires non-empty bucket");
    http::Headers hdrs;
    hdrs[userver::http::headers::kContentType] = std::string(contentType);
    // we use UNSIGNED-PAYLOAD for presign
    static_cast<void>(data);
    auto method = "PUT"_t;
    auto pathText = String::fromBytesThrow(path);
    auto protocolText = String::fromBytesThrow(protocol);
    auto urlText = presignVirtualHost(method, pathText, expiresAt, protocolText, std::move(hdrs));
    return std::string(urlText.view());
}

std::chrono::seconds S3V4Client::computePresignTtl(
    const std::chrono::system_clock::time_point &now,
    const std::chrono::system_clock::time_point &expiresAt
)
{
    auto ttl = std::chrono::duration_cast<std::chrono::seconds>(expiresAt - now);
    if (ttl.count() <= 0)
        ttl = std::chrono::seconds(1);
    if (ttl.count() > 604800)
        ttl = std::chrono::seconds(604800);
    return ttl;
}

SigV4Params S3V4Client::makeSigV4Params(const std::chrono::system_clock::time_point &now) const
{
    return {
        std::string(config.region.view()),
        "s3",
        creds.accessKeyId,
        creds.secretAccessKey,
        creds.sessionToken,
        now
    };
}

void S3V4Client::signRequest(
    String method, String canonicalUri, String host, http::Headers &headers,
    const String &payloadHash
) const
{
    const auto now = userver::utils::datetime::Now();
    const auto params = makeSigV4Params(now);
    auto prepared = prepareSignedHeaders(std::string(host.view()), headers);

    std::vector<std::pair<String, String>> headersText;
    headersText.reserve(prepared.size());
    for (const auto &kv : prepared) {
        headersText.emplace_back(
            String::fromBytesThrow(kv.first), String::fromBytesThrow(kv.second)
        );
    }

    auto signedMap = signHeaders(params, method, canonicalUri, {}, headersText, payloadHash);
    for (const auto &kv : signedMap)
        headers[kv.first] = kv.second;
}

String S3V4Client::buildRawPath(String path, bool includeBucket) const
{
    const auto basePath = std::string(endpoint.basePath.view());
    const auto bucket = std::string(bucketName.view());
    const auto pathBytes = std::string(path.view());

    std::string raw;
    raw.reserve(basePath.size() + bucket.size() + pathBytes.size() + 2);
    raw.append(basePath);
    if (raw.empty())
        raw.push_back('/');
    if (!raw.empty() && raw.back() != '/')
        raw.push_back('/');

    if (includeBucket && !bucket.empty()) {
        raw.append(bucket);
        if (!pathBytes.empty())
            raw.push_back('/');
    }

    raw.append(pathBytes);
    if (raw.empty() || raw.front() != '/')
        raw.insert(raw.begin(), '/');

    return String::fromBytesThrow(raw);
}

detail::BuiltUrl
S3V4Client::makePathStyleUrl(String path, std::optional<String> protocolOverride) const
{
    detail::BuiltUrl out;
    out.rawPath = buildRawPath(std::move(path), /*includeBucket*/ true);

    auto url = endpoint.url.copyParsed();
    if (protocolOverride) {
        const auto proto = std::string(protocolOverride->view());
        UINVARIANT(url.set_protocol(proto), "invalid protocol override");
    }
    UINVARIANT(url.set_pathname(out.rawPath.view()), "failed to set path for S3 request");
    url.set_search("");
    url.clear_hash();

    out.host = endpoint.host;
    out.href = String::fromBytesThrow(url.get_href());
    return out;
}

detail::BuiltUrl S3V4Client::makeVirtualHostUrl(String path, String protocol) const
{
    detail::BuiltUrl out;
    out.rawPath = buildRawPath(std::move(path), /*includeBucket*/ false);

    auto url = endpoint.url.copyParsed();
    const auto proto = std::string(protocol.view());
    UINVARIANT(url.set_protocol(proto), "invalid protocol for S3 presign");
    UINVARIANT(
        url.set_hostname(fmt::format("{}.{}", bucketName.view(), endpoint.hostname.view())),
        "bad hostname"
    );
    if (!endpoint.port.empty())
        UINVARIANT(url.set_port(std::string(endpoint.port.view())), "bad port");
    else
        url.set_port("");
    UINVARIANT(url.set_pathname(out.rawPath.view()), "failed to set path for S3 presign");
    url.set_search("");
    url.clear_hash();
    out.host = String::fromBytesThrow(url.get_host());
    out.href = String::fromBytesThrow(url.get_href());
    return out;
}

String S3V4Client::presignVirtualHost(
    String method, String path, const std::chrono::system_clock::time_point &expiresAt,
    String protocol, std::optional<http::Headers> extraHeaders
) const
{
    const auto now = userver::utils::datetime::Now();
    const auto built = makeVirtualHostUrl(std::move(path), std::move(protocol));

    const SigV4Params params = makeSigV4Params(now);

    http::Headers extra = extraHeaders.value_or(http::Headers{});
    auto prepared = prepareSignedHeaders(std::string(built.host.view()), extra);

    std::vector<std::pair<String, String>> headersText;
    headersText.reserve(prepared.size());
    for (const auto &kv : prepared) {
        headersText.emplace_back(
            String::fromBytesThrow(kv.first), String::fromBytesThrow(kv.second)
        );
    }

    return buildPresignedUrl(method, built, now, expiresAt, params, headersText);
}

String S3V4Client::presignPathStyle(
    String method, String path, const std::chrono::system_clock::time_point &expiresAt,
    String protocol
) const
{
    auto now = userver::utils::datetime::Now();
    auto built = makePathStyleUrl(std::move(path), std::move(protocol));
    SigV4Params params = makeSigV4Params(now);
    auto prepared = prepareSignedHeaders(std::string(built.host.view()), http::Headers{});

    std::vector<std::pair<String, String>> headersText;
    headersText.reserve(prepared.size());
    for (const auto &kv : prepared) {
        headersText.emplace_back(
            String::fromBytesThrow(kv.first), String::fromBytesThrow(kv.second)
        );
    }

    return buildPresignedUrl(method, built, now, expiresAt, params, headersText);
}

String S3V4Client::buildPresignedUrl(
    String method, const detail::BuiltUrl &built, const std::chrono::system_clock::time_point &now,
    const std::chrono::system_clock::time_point &expiresAt, const SigV4Params &params,
    const std::vector<std::pair<String, String>> &headers
) const
{
    const std::string scope = buildScope(params);
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
    query.emplace_back(
        "X-Amz-Credential", fmt::format("{}/{}", params.accessKeyId.GetUnderlying(), scope)
    );
    query.emplace_back("X-Amz-Date", params.amzDate);
    query.emplace_back("X-Amz-Expires", std::to_string(computePresignTtl(now, expiresAt).count()));

    std::vector<std::pair<std::string, std::string>> headersUtf8;
    headersUtf8.reserve(headers.size());
    for (const auto &kv : headers)
        headersUtf8.emplace_back(std::string(kv.first.view()), std::string(kv.second.view()));
    const std::string signedHeaders = buildSignedHeaders(headersUtf8);
    query.emplace_back("X-Amz-SignedHeaders", signedHeaders);
    if (params.sessionToken)
        query.emplace_back(
            "X-Amz-Security-Token", std::string(params.sessionToken->GetUnderlying().view())
        );

    const auto cr = buildCanonicalRequest(
        method.view(), built.rawPath.view(), query, headersUtf8, "UNSIGNED-PAYLOAD"
    );
    const std::string stringToSign = fmt::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", params.amzDate, scope, sha256Hex(cr.canonicalRequest)
    );
    const std::string signature = computeSignature(params, stringToSign);
    query.emplace_back("X-Amz-Signature", signature);

    auto url = std::string(built.href.view());
    url.push_back('?');
    url.append(canonicalizeQuery(query));
    return String::fromBytesThrow(url);
}

} // namespace v1::s3v4
