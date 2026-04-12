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
#include <format>
#include <iterator>
#include <utility>

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
namespace us = userver;
using namespace text::literals;

namespace v1::s3v4 {

namespace detail {

EndpointParts parseEndpoint(const String &ep)
{
    const auto link = Link::fromText(ep, ep.sizeBytes(), Link::FromTextOptions::kNone).expect();
    const auto &url = link.url;

    UINVARIANT(url.isHttp() || url.isHttps(), "S3 endpoint must be http or https");

    UINVARIANT(!url.hasSearch(), "S3 endpoint must not include query");
    std::string path{url.pathname().view()};
    if (path.empty())
        path = "/";
    UINVARIANT(path == "/", "S3 endpoint path must be root");

    return {url, url.host(), url.hostname(), url.hasPort() ? url.port() : String{}, "/"_t};
}
} // namespace detail

S3V4Client::S3V4Client(
    http::Client &httpClient, S3V4Config config, S3Credentials creds, String bucketName
)
    : httpClient(httpClient), config(std::move(config)), creds(std::move(creds)),
      bucketName(std::move(bucketName)), endpoint(detail::parseEndpoint(this->config.endpoint))
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
    const auto pathText = String::fromBytes(std::string(path)).expect();
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

    const std::string url{built.href.view()};
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
    const auto pathText = String::fromBytes(std::string(path)).expect();
    const auto built = makePathStyleUrl(pathText, {});
    http::Headers headers;
    const auto payloadHash = sha256Hex("");
    signRequest("DELETE"_t, built.rawPath, built.host, headers, payloadHash);

    const std::string url{built.href.view()};
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
    const auto pathText = String::fromBytes(std::string(path)).expect();
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

// Unused by this service; provide hard-fail stubs to satisfy the interface.
std::optional<std::string> S3V4Client::GetObject(
    std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &
) const
{
    us::utils::AbortWithStacktrace("GetObject not implemented in SigV4 client for this service");
}

std::string S3V4Client::TryGetObject(
    std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &
) const
{
    us::utils::AbortWithStacktrace("TryGetObject not implemented in SigV4 client");
}

std::optional<std::string> S3V4Client::GetPartialObject(
    std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *,
    const HeaderDataRequest &
) const
{
    us::utils::AbortWithStacktrace("GetPartialObject not implemented in SigV4 client");
}

std::string S3V4Client::TryGetPartialObject(
    std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *,
    const HeaderDataRequest &
) const
{
    us::utils::AbortWithStacktrace("TryGetPartialObject not implemented in SigV4 client");
}

std::string S3V4Client::CopyObject(
    std::string_view, std::string_view, std::string_view, const std::optional<Meta> &
)
{
    us::utils::AbortWithStacktrace("CopyObject not implemented in SigV4 client");
}

std::string S3V4Client::CopyObject(std::string_view, std::string_view, const std::optional<Meta> &)
{
    us::utils::AbortWithStacktrace("CopyObject not implemented in SigV4 client");
}

std::optional<std::string>
S3V4Client::ListBucketContents(std::string_view, int, std::string, std::string) const
{
    us::utils::AbortWithStacktrace("ListBucketContents not implemented in SigV4 client");
}

std::vector<s3::ObjectMeta> S3V4Client::ListBucketContentsParsed(std::string_view) const
{
    us::utils::AbortWithStacktrace("ListBucketContentsParsed not implemented in SigV4 client");
}

std::vector<std::string> S3V4Client::ListBucketDirectories(std::string_view) const
{
    us::utils::AbortWithStacktrace("ListBucketDirectories not implemented in SigV4 client");
}

void S3V4Client::UpdateConfig(s3::ConnectionCfg &&) {}

std::string_view S3V4Client::GetBucketName() const { return bucketName.view(); }

// Multipart upload API stubs (not used by this service)
userver::s3api::multipart_upload::InitiateMultipartUploadResult S3V4Client::CreateMultipartUpload(
    const userver::s3api::multipart_upload::CreateMultipartUploadRequest &
) const
{
    us::utils::AbortWithStacktrace("CreateMultipartUpload not implemented in SigV4 client");
}

userver::s3api::multipart_upload::UploadPartResult
S3V4Client::UploadPart(const userver::s3api::multipart_upload::UploadPartRequest &) const
{
    us::utils::AbortWithStacktrace("UploadPart not implemented in SigV4 client");
}

userver::s3api::multipart_upload::CompleteMultipartUploadResult S3V4Client::CompleteMultipartUpload(
    const userver::s3api::multipart_upload::CompleteMultipartUploadRequest &
) const
{
    us::utils::AbortWithStacktrace("CompleteMultipartUpload not implemented in SigV4 client");
}

void S3V4Client::AbortMultipartUpload(
    const userver::s3api::multipart_upload::AbortMultipartUploadRequest &
) const
{
    us::utils::AbortWithStacktrace("AbortMultipartUpload not implemented in SigV4 client");
}

userver::s3api::multipart_upload::ListPartsResult
S3V4Client::ListParts(const userver::s3api::multipart_upload::ListPartsRequest &) const
{
    us::utils::AbortWithStacktrace("ListParts not implemented in SigV4 client");
}

userver::s3api::multipart_upload::ListMultipartUploadsResult S3V4Client::ListMultipartUploads(
    const userver::s3api::multipart_upload::ListMultipartUploadsRequest &
) const
{
    us::utils::AbortWithStacktrace("ListMultipartUploads not implemented in SigV4 client");
}

// Presign support using chrono
std::string
S3V4Client::GenerateDownloadUrl(std::string_view path, time_t expiresEpoch, bool useSsl) const
{
    const auto expiresAt = std::chrono::system_clock::from_time_t(expiresEpoch);
    const auto method = "GET"_t;
    const auto pathText = String::fromBytes(path).expect();
    const auto protocolText = useSsl ? "https"_t : "http"_t;
    const auto urlText = presignPathStyle(method, pathText, expiresAt, protocolText);
    return std::string{urlText.view()};
}

std::string S3V4Client::GenerateDownloadUrlVirtualHostAddressing(
    std::string_view path, const std::chrono::system_clock::time_point &expiresAt,
    std::string_view protocol
) const
{
    UINVARIANT(!bucketName.empty(), "presign requires non-empty bucket");
    auto method = "GET"_t;
    auto pathText = String::fromBytes(path).expect();
    auto protocolText = String::fromBytes(protocol).expect();
    return std::string{presignVirtualHost(method, pathText, expiresAt, protocolText, {}).view()};
}

std::string S3V4Client::GenerateUploadUrlVirtualHostAddressing(
    std::string_view data, std::string_view contentType, std::string_view path,
    const std::chrono::system_clock::time_point &expiresAt, std::string_view protocol
) const
{
    UINVARIANT(!bucketName.empty(), "presign requires non-empty bucket");
    http::Headers hdrs;
    hdrs[userver::http::headers::kContentType] = std::string(contentType);
    // we use UNSIGNED-PAYLOAD for presign
    static_cast<void>(data);
    auto method = "PUT"_t;
    auto pathText = String::fromBytes(path).expect();
    auto protocolText = String::fromBytes(protocol).expect();
    auto urlText = presignVirtualHost(method, pathText, expiresAt, protocolText, std::move(hdrs));
    return std::string{urlText.view()};
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
            String::fromBytes(kv.first).expect(), String::fromBytes(kv.second).expect()
        );
    }

    auto signedMap = signHeaders(params, method, canonicalUri, {}, headersText, payloadHash);
    for (const auto &kv : signedMap)
        headers[kv.first] = kv.second;
}

String S3V4Client::buildRawPath(String path, IncludeBucket includeBucket) const
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

    if (includeBucket == IncludeBucket::kYes && !bucket.empty()) {
        raw.append(bucket);
        if (!pathBytes.empty())
            raw.push_back('/');
    }

    raw.append(pathBytes);
    if (raw.empty() || raw.front() != '/')
        raw.insert(raw.begin(), '/');

    return String::fromBytes(raw).expect();
}

detail::BuiltUrl
S3V4Client::makePathStyleUrl(String path, std::optional<String> protocolOverride) const
{
    detail::BuiltUrl out;
    out.rawPath = buildRawPath(std::move(path), IncludeBucket::kYes);

    auto url = endpoint.url.copyParsed();
    if (protocolOverride) {
        const auto proto = std::string(protocolOverride->view());
        UINVARIANT(url.set_protocol(proto), "invalid protocol override");
    }
    UINVARIANT(url.set_pathname(out.rawPath.view()), "failed to set path for S3 request");
    url.set_search("");
    url.clear_hash();

    out.host = endpoint.host;
    out.href = String::fromBytes(url.get_href()).expect();
    return out;
}

detail::BuiltUrl S3V4Client::makeVirtualHostUrl(String path, String protocol) const
{
    detail::BuiltUrl out;
    out.rawPath = buildRawPath(std::move(path), IncludeBucket::kNo);

    auto url = endpoint.url.copyParsed();
    const auto proto = std::string(protocol.view());
    UINVARIANT(url.set_protocol(proto), "invalid protocol for S3 presign");
    UINVARIANT(
        url.set_hostname(std::format("{}.{}", bucketName.view(), endpoint.hostname.view())),
        "bad hostname"
    );
    if (!endpoint.port.empty())
        UINVARIANT(url.set_port(std::string(endpoint.port.view())), "bad port");
    else
        url.set_port("");
    UINVARIANT(url.set_pathname(out.rawPath.view()), "failed to set path for S3 presign");
    url.set_search("");
    url.clear_hash();
    out.host = String::fromBytes(url.get_host()).expect();
    out.href = String::fromBytes(url.get_href()).expect();
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
            String::fromBytes(kv.first).expect(), String::fromBytes(kv.second).expect()
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
            String::fromBytes(kv.first).expect(), String::fromBytes(kv.second).expect()
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
        "X-Amz-Credential", std::format("{}/{}", params.accessKeyId.GetUnderlying(), scope)
    );
    query.emplace_back("X-Amz-Date", params.amzDate);
    query.emplace_back(
        "X-Amz-Expires", std::format("{}", computePresignTtl(now, expiresAt).count())
    );

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
    const std::string stringToSign = std::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", params.amzDate, scope, sha256Hex(cr.canonicalRequest)
    );
    const std::string signature = computeSignature(params, stringToSign);
    query.emplace_back("X-Amz-Signature", signature);

    auto url = std::string(built.href.view());
    url.push_back('?');
    url.append(canonicalizeQuery(query));
    return String::fromBytes(url).expect();
}

} // namespace v1::s3v4
