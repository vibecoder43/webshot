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

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include <userver/crypto/hash.hpp>

#include <userver/clients/http/client.hpp>
#include <userver/clients/http/response.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime.hpp>

namespace s3 = userver::s3api;
namespace http = userver::clients::http;

namespace v1::s3v4 {

namespace detail {

EndpointParts ParseEndpoint(const std::string &ep)
{
    const auto link = Link::fromUserInput(ep, ep.size());
    const auto &url = link.url;

    UINVARIANT(
        url.type == ada::scheme::type::HTTP || url.type == ada::scheme::type::HTTPS,
        "S3 endpoint must be http or https"
    );

    UINVARIANT(!url.has_search(), "S3 endpoint must not include query");
    std::string path = std::string(url.get_pathname());
    if (path.empty())
        path = "/";
    UINVARIANT(path == "/", "S3 endpoint path must be root");

    EndpointParts out;
    out.url = url;
    out.host = std::string(out.url.get_host());
    out.hostname = std::string(out.url.get_hostname());
    out.port = out.url.has_port() ? std::string(out.url.get_port()) : std::string{};
    out.basePath = "/";
    return out;
}

/** Lowercase header keys and add explicit `host` entry. */
std::vector<std::pair<std::string, std::string>>
PrepareSignedHeaders(std::string host, const http::Headers &extra)
{
    std::vector<std::pair<std::string, std::string>> v;
    v.reserve(extra.size() + 1);
    v.emplace_back("host", std::move(host));
    for (const auto &kv : extra) {
        std::string keyLower;
        keyLower.reserve(kv.first.size());
        for (char c : kv.first)
            keyLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        v.emplace_back(std::move(keyLower), kv.second);
    }
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    return v;
}
} // namespace detail

S3V4Client::S3V4Client(
    http::Client &http, S3V4Config cfg, S3Credentials creds, std::string defaultBucket
)
    : http_(http), cfg_(std::move(cfg)), creds_(std::move(creds)),
      bucket_(std::move(defaultBucket)), ep_(detail::ParseEndpoint(cfg_.endpoint))
{
}

// methods used by this service
std::string S3V4Client::PutObject(
    std::string_view path, std::string data, const std::optional<Meta> &meta,
    std::string_view content_type, const std::optional<std::string> &content_disposition,
    const std::optional<std::vector<Tag>> &tags
) const
{
    (void)tags; // not used in current service
    const auto built = MakePathStyleUrl(path);
    http::Headers headers;
    headers[userver::http::headers::kContentType] = std::string(content_type);
    if (content_disposition)
        headers[userver::http::headers::kContentDisposition] = *content_disposition;
    if (meta) {
        for (const auto &kv : *meta)
            headers["x-amz-meta-" + kv.first] = kv.second;
    }
    const std::string payload_hash = Sha256Hex(data);
    SignRequest("PUT", built.rawPath, built.host, headers, payload_hash);

    const std::string url = built.href;
    auto resp = http_.CreateNotSignedRequest()
                    .put(url, std::move(data))
                    .headers(headers)
                    .timeout(cfg_.timeout)
                    .perform();
    resp->raise_for_status();
    return resp->body();
}

void S3V4Client::DeleteObject(std::string_view path) const
{
    const auto built = MakePathStyleUrl(path);
    http::Headers headers;
    const std::string payload_hash = Sha256Hex("");
    SignRequest("DELETE", built.rawPath, built.host, headers, payload_hash);

    const std::string url = built.href;
    auto resp = http_.CreateNotSignedRequest()
                    .delete_method(url)
                    .headers(headers)
                    .timeout(cfg_.timeout)
                    .perform();
    resp->raise_for_status();
}

// Minimal header HEAD support (used indirectly by copy in userver, not used here)
std::optional<s3::Client::HeadersDataResponse>
S3V4Client::GetObjectHead(std::string_view path, const HeaderDataRequest &request) const
{
    const auto built = MakePathStyleUrl(path);
    http::Headers headers;
    const std::string payload_hash = Sha256Hex("");
    SignRequest("HEAD", built.rawPath, built.host, headers, payload_hash);
    const std::string url = built.href;
    auto resp =
        http_.CreateNotSignedRequest().head(url).headers(headers).timeout(cfg_.timeout).perform();
    resp->raise_for_status();
    HeadersDataResponse out;
    if (request.need_meta) {
        out.meta.emplace();
        for (const auto &kv : resp->headers()) {
            const auto &name = kv.first;
            if (name.size() > 11 &&
                std::equal(name.begin(), name.begin() + 11, "x-amz-meta-", [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == b;
                })) {
                out.meta->emplace(name.substr(11), kv.second);
            }
        }
    }
    if (request.headers) {
        out.headers.emplace();
        for (const auto &wanted : *request.headers) {
            auto it = resp->headers().find(wanted);
            if (it != resp->headers().end())
                out.headers->emplace(it->first, it->second);
        }
    }
    return out;
}

// Unused by this service — provide stubs that throw to make it explicit
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

std::string_view S3V4Client::GetBucketName() const { return bucket_; }

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
S3V4Client::GenerateDownloadUrl(std::string_view path, time_t expires_epoch, bool use_ssl) const
{
    const auto expires_at = std::chrono::system_clock::from_time_t(expires_epoch);
    const auto protocol = use_ssl ? std::string{"https"} : std::string{"http"};
    return PresignPathStyle("GET", path, expires_at, protocol);
}

std::string S3V4Client::GenerateDownloadUrlVirtualHostAddressing(
    std::string_view path, const std::chrono::system_clock::time_point &expires_at,
    std::string_view protocol
) const
{
    if (bucket_.empty())
        throw std::runtime_error("presign requires non-empty bucket");
    return PresignVirtualHost("GET", path, expires_at, protocol, std::nullopt);
}

std::string S3V4Client::GenerateUploadUrlVirtualHostAddressing(
    std::string_view data, std::string_view content_type, std::string_view path,
    const std::chrono::system_clock::time_point &expires_at, std::string_view protocol
) const
{
    if (bucket_.empty())
        throw std::runtime_error("presign requires non-empty bucket");
    http::Headers hdrs;
    hdrs[userver::http::headers::kContentType] = std::string(content_type);
    (void)data; // we use UNSIGNED-PAYLOAD for presign
    return PresignVirtualHost("PUT", path, expires_at, protocol, std::move(hdrs));
}

std::chrono::seconds S3V4Client::ComputePresignTtl(
    const std::chrono::system_clock::time_point &now,
    const std::chrono::system_clock::time_point &expires_at
)
{
    auto ttl = std::chrono::duration_cast<std::chrono::seconds>(expires_at - now);
    if (ttl.count() <= 0)
        ttl = std::chrono::seconds(1);
    if (ttl.count() > 604800)
        ttl = std::chrono::seconds(604800);
    return ttl;
}

SigV4Params S3V4Client::MakeSigV4Params(const std::chrono::system_clock::time_point &now) const
{
    SigV4Params params;
    params.region = cfg_.region;
    params.accessKeyId = creds_.accessKeyId;
    params.secretAccessKey = creds_.secretAccessKey;
    params.sessionToken = creds_.sessionToken;
    params.amzDate = ToAmzDateUtc(now);
    params.date = ToDateStampUtc(now);
    return params;
}

void S3V4Client::SignRequest(
    std::string_view method, std::string_view canonicalUri, std::string_view host,
    http::Headers &headers, const std::string &payload_hash
) const
{
    const auto now = userver::utils::datetime::Now();
    const SigV4Params params = MakeSigV4Params(now);

    auto to_sign = detail::PrepareSignedHeaders(std::string(host), headers);
    auto signed_map = SignHeaders(
        params, method, canonicalUri, /*query*/ {}, to_sign, payload_hash
    );
    for (const auto &kv : signed_map)
        headers[kv.first] = kv.second;
}

std::string S3V4Client::BuildRawPath(std::string_view path, bool includeBucket) const
{
    std::string raw;
    raw.reserve(ep_.basePath.size() + bucket_.size() + path.size() + 2);

    raw.append(ep_.basePath);
    if (raw.empty())
        raw.push_back('/');
    if (!raw.empty() && raw.back() != '/')
        raw.push_back('/');

    if (includeBucket && !bucket_.empty()) {
        raw.append(bucket_);
        if (!path.empty())
            raw.push_back('/');
    }

    raw.append(path);
    if (raw.empty() || raw.front() != '/')
        raw.insert(raw.begin(), '/');
    return raw;
}

detail::BuiltUrl S3V4Client::MakePathStyleUrl(
    std::string_view path, std::optional<std::string_view> protocolOverride
) const
{
    detail::BuiltUrl out;
    out.rawPath = BuildRawPath(path, /*includeBucket*/ true);

    auto url = ep_.url;
    if (protocolOverride)
        UINVARIANT(url.set_protocol(*protocolOverride), "invalid protocol override");
    UINVARIANT(url.set_pathname(out.rawPath), "failed to set path for S3 request");
    url.set_search("");
    url.clear_hash();

    out.host = ep_.host;
    out.href = std::string(url.get_href());
    return out;
}

detail::BuiltUrl
S3V4Client::MakeVirtualHostUrl(std::string_view path, std::string_view protocol) const
{
    detail::BuiltUrl out;
    out.rawPath = BuildRawPath(path, /*includeBucket*/ false);

    auto url = ep_.url;
    UINVARIANT(url.set_protocol(protocol), "invalid protocol for S3 presign");
    UINVARIANT(url.set_hostname(fmt::format("{}.{}", bucket_, ep_.hostname)), "bad hostname");
    if (!ep_.port.empty())
        UINVARIANT(url.set_port(ep_.port), "bad port");
    else
        url.set_port("");
    UINVARIANT(url.set_pathname(out.rawPath), "failed to set path for S3 presign");
    url.set_search("");
    url.clear_hash();

    out.host = std::string(url.get_host());
    out.href = std::string(url.get_href());
    return out;
}

std::string S3V4Client::CanonicalizeQuery(std::vector<std::pair<std::string, std::string>> q)
{
    for (auto &kv : q) {
        kv.first = PercentEncode(kv.first, /*encodeSlash*/ true);
        kv.second = PercentEncode(kv.second, /*encodeSlash*/ true);
    }
    std::sort(q.begin(), q.end(), [](const auto &a, const auto &b) {
        if (a.first == b.first)
            return a.second < b.second;
        return a.first < b.first;
    });
    std::ostringstream oss;
    for (std::size_t i = 0; i < q.size(); i++) {
        if (i)
            oss << '&';
        oss << q[i].first << '=' << q[i].second;
    }
    return oss.str();
}

std::string S3V4Client::PresignVirtualHost(
    std::string_view method, std::string_view path,
    const std::chrono::system_clock::time_point &expires_at, std::string_view protocol,
    std::optional<http::Headers> extra_headers
) const
{
    const auto now = userver::utils::datetime::Now();
    const auto ttl = ComputePresignTtl(now, expires_at);

    const auto built = MakeVirtualHostUrl(path, protocol);

    const SigV4Params params = MakeSigV4Params(now);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("host", built.host);
    if (extra_headers) {
        for (const auto &kv : *extra_headers) {
            std::string k;
            k.reserve(kv.first.size());
            for (char c : kv.first)
                k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            headers.emplace_back(std::move(k), kv.second);
        }
    }
    std::sort(headers.begin(), headers.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });

    const std::string scope = fmt::format(
        "{}/{}/{}/aws4_request", params.date, params.region, params.service
    );
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
    query.emplace_back(
        "X-Amz-Credential", fmt::format("{}/{}", params.accessKeyId.GetUnderlying(), scope)
    );
    query.emplace_back("X-Amz-Date", params.amzDate);
    query.emplace_back("X-Amz-Expires", std::to_string(ttl.count()));
    std::ostringstream sh;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (i)
            sh << ';';
        sh << headers[i].first;
    }
    const auto signed_headers = sh.str();
    query.emplace_back("X-Amz-SignedHeaders", signed_headers);
    if (params.sessionToken)
        query.emplace_back("X-Amz-Security-Token", params.sessionToken->GetUnderlying());

    const auto cr = BuildCanonicalRequest(
        method, built.rawPath, query, headers, std::string("UNSIGNED-PAYLOAD")
    );
    const std::string string_to_sign = fmt::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", params.amzDate, scope, Sha256Hex(cr.canonicalRequest)
    );
    const std::string signature = ComputePresignSignature(params, string_to_sign);
    query.emplace_back("X-Amz-Signature", signature);

    std::string url = built.href;
    url.push_back('?');
    url.append(CanonicalizeQuery(std::move(query)));
    return url;
}

std::string S3V4Client::PresignPathStyle(
    std::string_view method, std::string_view path,
    const std::chrono::system_clock::time_point &expires_at, std::string_view protocol
) const
{
    const auto now = userver::utils::datetime::Now();
    const auto ttl = ComputePresignTtl(now, expires_at);

    const auto built = MakePathStyleUrl(path, protocol);

    const SigV4Params params = MakeSigV4Params(now);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("host", built.host);
    std::sort(headers.begin(), headers.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });

    const std::string scope = fmt::format(
        "{}/{}/{}/aws4_request", params.date, params.region, params.service
    );
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
    query.emplace_back(
        "X-Amz-Credential", fmt::format("{}/{}", params.accessKeyId.GetUnderlying(), scope)
    );
    query.emplace_back("X-Amz-Date", params.amzDate);
    query.emplace_back("X-Amz-Expires", std::to_string(ttl.count()));
    query.emplace_back("X-Amz-SignedHeaders", std::string("host"));
    if (params.sessionToken)
        query.emplace_back("X-Amz-Security-Token", params.sessionToken->GetUnderlying());

    const auto cr = BuildCanonicalRequest(
        method, built.rawPath, query, headers, std::string("UNSIGNED-PAYLOAD")
    );
    const std::string string_to_sign = fmt::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", params.amzDate, scope, Sha256Hex(cr.canonicalRequest)
    );
    const std::string signature = ComputePresignSignature(params, string_to_sign);
    query.emplace_back("X-Amz-Signature", signature);

    std::string url = built.href;
    url.push_back('?');
    url.append(CanonicalizeQuery(std::move(query)));
    return url;
}

std::string
S3V4Client::ComputePresignSignature(const SigV4Params &params, const std::string &string_to_sign)
{
    namespace US = USERVER_NAMESPACE::crypto::hash;
    const std::string kSecret = fmt::format("AWS4{}", params.secretAccessKey.GetUnderlying());
    const std::string kDate = US::HmacSha256(kSecret, params.date, US::OutputEncoding::kBinary);
    const std::string kRegion = US::HmacSha256(kDate, params.region, US::OutputEncoding::kBinary);
    const std::string kService = US::HmacSha256(
        kRegion, params.service, US::OutputEncoding::kBinary
    );
    const std::string kSigning = US::HmacSha256(
        kService, "aws4_request", US::OutputEncoding::kBinary
    );
    return US::HmacSha256(kSigning, string_to_sign, US::OutputEncoding::kHex);
}

} // namespace v1::s3v4
