/**
 * @file
 * @brief Minimal S3 client that signs requests with AWS Signature V4.
 *
 * Implements the subset of methods required by this service (PUT, HEAD, DELETE)
 * and presign helpers. It relies on userver HTTP client for transport.
 */
#include "s3/s3_v4_client.hpp"
#include "s3/s3_url_utils.hpp"
#include "s3/sigv4_signer.hpp"

#include "integers.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>
#include <vector>

#include <userver/crypto/hash.hpp>

#include <userver/clients/http/client.hpp>
#include <userver/clients/http/response.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/text_light.hpp>

namespace s3 = us::s3api;
using namespace text::literals;

namespace v1::s3v4 {
using namespace std::chrono_literals;

namespace detail {

constexpr std::chrono::seconds kMinPresignTtl = 1s;
constexpr std::chrono::seconds kMaxPresignTtl = 604800s;

[[nodiscard]] std::vector<std::pair<String, String>>
toTextPairs(const std::vector<std::pair<std::string, std::string>> &pairs)
{
    std::vector<std::pair<String, String>> out;
    out.reserve(pairs.size());
    for (const auto &[name, value] : pairs) {
        out.emplace_back(String::fromBytes(name).expect(), String::fromBytes(value).expect());
    }
    return out;
}

EndpointParts parseEndpoint(const String &ep)
{
    const auto url = parseUrlWithDefaultHttpScheme(ep);
    invariant(url, "S3 endpoint must parse");

    invariant(url->isHttpOrHttps(), "S3 endpoint must be http or https");

    invariant(!url->hasSearch(), "S3 endpoint must not include query");
    std::string path{url->pathname().view()};
    if (path.empty())
        path = "/";
    invariant(path == "/", "S3 endpoint path must be root");

    return {*url, url->host(), url->hostname(), url->hasPort() ? url->port() : String{}, "/"_t};
}

Expected<void, VirtualHostPresignError> validateVirtualHostBucketName(const String &bucketName)
{
    if (bucketName.empty())
        return Unex(VirtualHostPresignError::kMissingBucket);
    return {};
}
} // namespace detail

S3V4Client::S3V4Client(
    httpc::Client &httpClient, S3V4Config config, S3Credentials creds, String bucketName
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
    const auto pathText = String::fromBytes(path).expect();
    const auto built = makePathStyleUrl(pathText, {});
    httpc::Headers headers;
    headers[us::http::headers::kContentType] = std::string(contentType);
    if (contentDisposition)
        headers[us::http::headers::kContentDisposition] = *contentDisposition;
    if (meta) {
        for (const auto &[name, value] : *meta)
            headers["x-amz-meta-" + name] = value;
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
    const auto pathText = String::fromBytes(path).expect();
    const auto built = makePathStyleUrl(pathText, {});
    httpc::Headers headers;
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
    const auto pathText = String::fromBytes(path).expect();
    const auto built = makePathStyleUrl(pathText, {});
    httpc::Headers headers;
    const auto payloadHash = sha256Hex("");
    signRequest("HEAD"_t, built.rawPath, built.host, headers, payloadHash);
    const auto url = std::to_string(built.href);
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
        for (const auto &[name, value] : resp->headers()) {
            if (us::utils::text::ICaseStartsWith(std::string_view{name}, kMetaPrefix)) {
                out.meta->emplace(name.substr(kMetaPrefix.size()), value);
            }
        }
    }
    if (request.headers) {
        out.headers.emplace();
        for (const auto &wanted : *request.headers) {
            auto it = resp->headers().find(wanted);
            if (it != resp->headers().end()) {
                const auto &[name, value] = *it;
                out.headers->emplace(name, value);
            }
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
us::s3api::multipart_upload::InitiateMultipartUploadResult S3V4Client::CreateMultipartUpload(
    const us::s3api::multipart_upload::CreateMultipartUploadRequest &
) const
{
    us::utils::AbortWithStacktrace("CreateMultipartUpload not implemented in SigV4 client");
}

us::s3api::multipart_upload::UploadPartResult
S3V4Client::UploadPart(const us::s3api::multipart_upload::UploadPartRequest &) const
{
    us::utils::AbortWithStacktrace("UploadPart not implemented in SigV4 client");
}

us::s3api::multipart_upload::CompleteMultipartUploadResult S3V4Client::CompleteMultipartUpload(
    const us::s3api::multipart_upload::CompleteMultipartUploadRequest &
) const
{
    us::utils::AbortWithStacktrace("CompleteMultipartUpload not implemented in SigV4 client");
}

void S3V4Client::AbortMultipartUpload(
    const us::s3api::multipart_upload::AbortMultipartUploadRequest &
) const
{
    us::utils::AbortWithStacktrace("AbortMultipartUpload not implemented in SigV4 client");
}

us::s3api::multipart_upload::ListPartsResult
S3V4Client::ListParts(const us::s3api::multipart_upload::ListPartsRequest &) const
{
    us::utils::AbortWithStacktrace("ListParts not implemented in SigV4 client");
}

us::s3api::multipart_upload::ListMultipartUploadsResult S3V4Client::ListMultipartUploads(
    const us::s3api::multipart_upload::ListMultipartUploadsRequest &
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
    httpc::Headers hdrs;
    hdrs[us::http::headers::kContentType] = std::string(contentType);
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
        ttl = kMinPresignTtl;
    if (ttl.count() > kMaxPresignTtl.count())
        ttl = kMaxPresignTtl;
    return ttl;
}

SigV4Params S3V4Client::makeSigV4Params(const std::chrono::system_clock::time_point &now) const
{
    return {
        std::to_string(config.region),
        "s3",
        creds.accessKeyId,
        creds.secretAccessKey,
        creds.sessionToken,
        now
    };
}

void S3V4Client::signRequest(
    String method, String canonicalUri, String host, httpc::Headers &headers,
    const String &payloadHash
) const
{
    const auto now = datetime::Now();
    const auto params = makeSigV4Params(now);
    auto prepared = prepareSignedHeaders(std::to_string(host), headers);
    const auto headersText = detail::toTextPairs(prepared);

    auto signedMap = signHeaders(params, method, canonicalUri, {}, headersText, payloadHash);
    for (const auto &[name, value] : signedMap)
        headers[name] = value;
}

String S3V4Client::buildRawPath(String path, IncludeBucket includeBucket) const
{
    const auto basePath = std::to_string(endpoint.basePath);
    const auto bucket = std::to_string(bucketName);
    const auto pathBytes = std::to_string(path);

    std::string raw;
    raw.reserve(numericCast<size_t>(ssize(basePath) + ssize(bucket) + ssize(pathBytes) + 2_i64));
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
    auto rawPath = buildRawPath(std::move(path), IncludeBucket::kYes);

    auto url = protocolOverride ? endpoint.url.withProtocol(*protocolOverride) : endpoint.url;
    url = url.withPathname(rawPath).withoutSearch().withoutHash();

    return detail::BuiltUrl{
        .href = url.href(),
        .host = endpoint.host,
        .rawPath = std::move(rawPath),
    };
}

detail::BuiltUrl S3V4Client::makeVirtualHostUrl(String path, String protocol) const
{
    const auto bucketValidated = detail::validateVirtualHostBucketName(bucketName);
    invariant(bucketValidated, "presign requires non-empty bucket");

    auto rawPath = buildRawPath(std::move(path), IncludeBucket::kNo);
    const auto hostname = text::format("{}.{}", bucketName, endpoint.hostname);
    auto url = endpoint.url.withProtocol(protocol).withHostname(hostname).withPort(endpoint.port);
    url = url.withPathname(rawPath).withoutSearch().withoutHash();
    return detail::BuiltUrl{
        .href = url.href(),
        .host = url.host(),
        .rawPath = std::move(rawPath),
    };
}

String S3V4Client::presignVirtualHost(
    String method, String path, const std::chrono::system_clock::time_point &expiresAt,
    String protocol, std::optional<httpc::Headers> extraHeaders
) const
{
    const auto now = datetime::Now();
    const auto built = makeVirtualHostUrl(std::move(path), std::move(protocol));

    const SigV4Params params = makeSigV4Params(now);

    httpc::Headers extra = extraHeaders.value_or(httpc::Headers{});
    auto prepared = prepareSignedHeaders(std::to_string(built.host), extra);
    const auto headersText = detail::toTextPairs(prepared);

    return buildPresignedUrl(method, built, now, expiresAt, params, headersText);
}

String S3V4Client::presignPathStyle(
    String method, String path, const std::chrono::system_clock::time_point &expiresAt,
    String protocol
) const
{
    auto now = datetime::Now();
    auto built = makePathStyleUrl(std::move(path), std::move(protocol));
    SigV4Params params = makeSigV4Params(now);
    auto prepared = prepareSignedHeaders(std::to_string(built.host), httpc::Headers{});
    const auto headersText = detail::toTextPairs(prepared);

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
    query.emplace_back("X-Amz-Expires", std::to_string(computePresignTtl(now, expiresAt).count()));

    std::vector<std::pair<std::string, std::string>> headersUtf8;
    headersUtf8.reserve(headers.size());
    for (const auto &[name, value] : headers)
        headersUtf8.emplace_back(std::to_string(name), std::to_string(value));
    const std::string signedHeaders = buildSignedHeaders(headersUtf8);
    query.emplace_back("X-Amz-SignedHeaders", signedHeaders);
    if (params.sessionToken)
        query.emplace_back(
            "X-Amz-Security-Token", std::to_string(params.sessionToken->GetUnderlying())
        );

    const auto cr = buildCanonicalRequest(
        method.view(), built.rawPath.view(), query, headersUtf8, "UNSIGNED-PAYLOAD"
    );
    const std::string stringToSign = std::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", params.amzDate, scope, sha256Hex(cr.canonicalRequest)
    );
    const std::string signature = computeSignature(params, stringToSign);
    query.emplace_back("X-Amz-Signature", signature);

    auto url = std::to_string(built.href);
    url.push_back('?');
    url.append(canonicalizeQuery(query));
    return String::fromBytes(url).expect();
}

} // namespace v1::s3v4
