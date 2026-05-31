/**
 * @file
 * @brief Minimal  client that signs requests with AWS Signature .
 *
 * Implements the subset of methods required by this service (PUT, HEAD, DELETE)
 * and presign helpers. It relies on userver HTTP client for transport.
 */
#include "s3/client.hpp"
#include "s3/sigv4_signer.hpp"
#include "s3/url_utils.hpp"

#include "integers.hpp"
#include "invariant.hpp"
#include "text.hpp"

#include <format>
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

namespace us = userver;
using namespace text::literals;

namespace ws::s3 {
namespace us = userver;
namespace datetime = us::utils::datetime;
namespace httpc = us::clients::http;
using namespace std::chrono_literals;

namespace detail {

constexpr std::chrono::seconds kMinPresignTtl = 1s;
constexpr std::chrono::seconds kMaxPresignTtl = 604800s;

EndpointParts ParseEndpoint(const String &ep)
{
    auto url = ParseUrlWithDefaultHttpScheme(ep);
    Invariant(url, " endpoint must parse"_t);

    Invariant(url->IsHttpOrHttps(), " endpoint must be http or https"_t);

    Invariant(!url->HasSearch(), " endpoint must not include query"_t);
    std::string path{url->Pathname().View()};
    if (path.empty())
        path = "/";
    Invariant(path == "/", " endpoint path must be root"_t);

    return {*url, url->Host(), url->Hostname(), url->HasPort() ? url->Port() : String{}, "/"_t};
}

Expected<void, VirtualHostPresignError> ValidateVirtualHostBucketName(const String &bucket_name)
{
    if (bucket_name.Empty())
        return Unex(VirtualHostPresignError::kMissingBucket);
    return {};
}
} // namespace detail

Client::Client(httpc::Client &http_client, Config config, Credentials creds, String bucket_name)
    : http_client_(http_client), config_(std::move(config)), creds_(std::move(creds)),
      bucket_name_(std::move(bucket_name)), endpoint_(detail::ParseEndpoint(config_.endpoint))
{
}

// methods used by this service
std::string Client::PutObject(
    std::string_view path, std::string data, const std::optional<Meta> &meta,
    std::string_view content_type, const std::optional<std::string> &content_disposition,
    const std::optional<std::vector<Tag>> &tags
) const
{
    static_cast<void>(tags);
    auto path_text = *String::FromBytes(path);
    auto built = MakePathStyleUrl(path_text, {});
    httpc::Headers headers;
    headers[us::http::headers::kContentType] = std::string(content_type);
    if (content_disposition)
        headers[us::http::headers::kContentDisposition] = *content_disposition;
    if (meta) {
        for (const auto &[name, value] : *meta)
            headers["x-amz-meta-" + name] = value;
    }
    const String payload_hash = Sha256Hex(data);
    SignRequest("PUT"_t, built.raw_path, built.host, headers, payload_hash);

    const std::string url{built.href.View()};
    auto resp = http_client_.CreateNotSignedRequest()
                    .put(url, std::move(data))
                    .headers(headers)
                    .timeout(config_.timeout)
                    .perform();
    resp->raise_for_status();
    return resp->body();
}

void Client::DeleteObject(std::string_view path) const
{
    auto path_text = *String::FromBytes(path);
    auto built = MakePathStyleUrl(path_text, {});
    httpc::Headers headers;
    auto payload_hash = Sha256Hex("");
    SignRequest("DELETE"_t, built.raw_path, built.host, headers, payload_hash);

    const std::string url{built.href.View()};
    auto resp = http_client_.CreateNotSignedRequest()
                    .delete_method(url)
                    .headers(headers)
                    .timeout(config_.timeout)
                    .perform();
    resp->raise_for_status();
}

// Minimal header HEAD support (used indirectly by copy in userver, not used here)
std::optional<s3::Client::HeadersDataResponse>
Client::GetObjectHead(std::string_view path, const HeaderDataRequest &request) const
{
    const auto path_text = *String::FromBytes(path);
    auto built = MakePathStyleUrl(path_text, {});
    httpc::Headers headers;
    const auto payload_hash = Sha256Hex("");
    SignRequest("HEAD"_t, built.raw_path, built.host, headers, payload_hash);
    const auto url = built.href.ToBytes();
    auto resp = http_client_.CreateNotSignedRequest()
                    .head(url)
                    .headers(headers)
                    .timeout(config_.timeout)
                    .perform();
    resp->raise_for_status();
    HeadersDataResponse out;
    if (request.need_meta) {
        static constexpr std::string_view meta_prefix = "x-amz-meta-";
        out.meta.emplace();
        for (const auto &[name, value] : resp->headers()) {
            if (us::utils::text::ICaseStartsWith(std::string_view{name}, meta_prefix)) {
                out.meta->emplace(name.substr(meta_prefix.size()), value);
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
std::optional<std::string> Client::GetObject(
    std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &
) const
{
    us::utils::AbortWithStacktrace("");
}

std::string Client::TryGetObject(
    std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &
) const
{
    us::utils::AbortWithStacktrace("");
}

std::optional<std::string> Client::GetPartialObject(
    std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *,
    const HeaderDataRequest &
) const
{
    us::utils::AbortWithStacktrace("");
}

std::string Client::TryGetPartialObject(
    std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *,
    const HeaderDataRequest &
) const
{
    us::utils::AbortWithStacktrace("");
}

std::string Client::CopyObject(
    std::string_view, std::string_view, std::string_view, const std::optional<Meta> &
)
{
    us::utils::AbortWithStacktrace("");
}

std::string Client::CopyObject(std::string_view, std::string_view, const std::optional<Meta> &)
{
    us::utils::AbortWithStacktrace("");
}

std::optional<std::string>
Client::ListBucketContents(std::string_view, int, std::string, std::string) const
{
    us::utils::AbortWithStacktrace("");
}

std::vector<us::s3api::ObjectMeta> Client::ListBucketContentsParsed(std::string_view) const
{
    us::utils::AbortWithStacktrace("");
}

std::vector<std::string> Client::ListBucketDirectories(std::string_view) const
{
    us::utils::AbortWithStacktrace("");
}

void Client::UpdateConfig(us::s3api::ConnectionCfg &&) {}

std::string_view Client::GetBucketName() const { return bucket_name_.View(); }

// Multipart upload API stubs (not used by this service)
us::s3api::multipart_upload::InitiateMultipartUploadResult Client::CreateMultipartUpload(
    const us::s3api::multipart_upload::CreateMultipartUploadRequest &
) const
{
    us::utils::AbortWithStacktrace("");
}

us::s3api::multipart_upload::UploadPartResult
Client::UploadPart(const us::s3api::multipart_upload::UploadPartRequest &) const
{
    us::utils::AbortWithStacktrace("");
}

us::s3api::multipart_upload::CompleteMultipartUploadResult Client::CompleteMultipartUpload(
    const us::s3api::multipart_upload::CompleteMultipartUploadRequest &
) const
{
    us::utils::AbortWithStacktrace("");
}

void Client::AbortMultipartUpload(
    const us::s3api::multipart_upload::AbortMultipartUploadRequest &
) const
{
    us::utils::AbortWithStacktrace("");
}

us::s3api::multipart_upload::ListPartsResult
Client::ListParts(const us::s3api::multipart_upload::ListPartsRequest &) const
{
    us::utils::AbortWithStacktrace("");
}

us::s3api::multipart_upload::ListMultipartUploadsResult
Client::ListMultipartUploads(const us::s3api::multipart_upload::ListMultipartUploadsRequest &) const
{
    us::utils::AbortWithStacktrace("");
}

// Presign support using chrono
std::string
Client::GenerateDownloadUrl(std::string_view path, time_t expires_epoch, bool use_ssl) const
{
    auto expires_at = std::chrono::system_clock::from_time_t(expires_epoch);
    auto method = "GET"_t;
    auto path_text = *String::FromBytes(path);
    auto protocol_text = use_ssl ? "https"_t : "http"_t;
    auto url_text = PresignPathStyle(method, path_text, expires_at, protocol_text);
    return std::string{url_text.View()};
}

std::string Client::GenerateDownloadUrlVirtualHostAddressing(
    std::string_view path, const std::chrono::system_clock::time_point &expires_at,
    std::string_view protocol
) const
{
    auto method = "GET"_t;
    auto path_text = *String::FromBytes(path);
    auto protocol_text = *String::FromBytes(protocol);
    return std::string{PresignVirtualHost(method, path_text, expires_at, protocol_text, {}).View()};
}

std::string Client::GenerateUploadUrlVirtualHostAddressing(
    std::string_view data, std::string_view content_type, std::string_view path,
    const std::chrono::system_clock::time_point &expires_at, std::string_view protocol
) const
{
    httpc::Headers hdrs;
    hdrs[us::http::headers::kContentType] = std::string(content_type);
    // we use UNSIGNED-PAYLOAD for presign
    static_cast<void>(data);
    auto method = "PUT"_t;
    auto path_text = *String::FromBytes(path);
    auto protocol_text = *String::FromBytes(protocol);
    auto url_text = PresignVirtualHost(
        method, path_text, expires_at, protocol_text, std::move(hdrs)
    );
    return std::string{url_text.View()};
}

std::chrono::seconds Client::ComputePresignTtl(
    const std::chrono::system_clock::time_point &now,
    const std::chrono::system_clock::time_point &expires_at
)
{
    auto ttl = std::chrono::duration_cast<std::chrono::seconds>(expires_at - now);
    if (ttl <= 0s)
        ttl = detail::kMinPresignTtl;
    if (ttl > detail::kMaxPresignTtl)
        ttl = detail::kMaxPresignTtl;
    return ttl;
}

SigParams Client::MakeSigParams(const std::chrono::system_clock::time_point &now) const
{
    return {config_.region.ToBytes(), "s3", creds_.access_key_id, creds_.secret_access_key,
            creds_.session_token,     now};
}

void Client::SignRequest(
    String method, String canonical_uri, String host, httpc::Headers &headers,
    const String &payload_hash
) const
{
    auto now = datetime::Now();
    auto params = MakeSigParams(now);
    auto prepared = PrepareSignedHeaders(host.ToBytes(), headers);
    auto headers_text = *text::StringPairs(prepared);

    auto signed_map = SignHeaders(params, method, canonical_uri, {}, headers_text, payload_hash);
    for (const auto &[name, value] : signed_map)
        headers[name] = value;
}

String Client::MakeRawPath(String path, IncludeBucket include_bucket) const
{
    auto base_path = endpoint_.base_path.ToBytes();
    auto bucket = bucket_name_.ToBytes();
    auto path_bytes = path.ToBytes();

    std::string raw;
    raw.reserve(NumericCast<size_t>(ssize(base_path) + ssize(bucket) + ssize(path_bytes) + 2_i64));
    raw.append(base_path);
    if (raw.empty())
        raw.push_back('/');
    if (!raw.empty() && raw.back() != '/')
        raw.push_back('/');

    if (include_bucket == IncludeBucket::kYes && !bucket.empty()) {
        raw.append(bucket);
        if (!path_bytes.empty())
            raw.push_back('/');
    }

    raw.append(path_bytes);
    if (raw.empty() || raw.front() != '/')
        raw.insert(raw.begin(), '/');

    return *String::FromBytes(raw);
}

detail::BuiltUrl
Client::MakePathStyleUrl(String path, std::optional<String> protocol_override) const
{
    auto raw_path = MakeRawPath(std::move(path), IncludeBucket::kYes);

    auto url = protocol_override ? endpoint_.url.WithProtocol(*protocol_override) : endpoint_.url;
    url = url.WithPathname(raw_path).Without(Url::StripOptions::kHash | Url::StripOptions::kQuery);

    return {
        .href = url.Href(),
        .host = endpoint_.host,
        .raw_path = std::move(raw_path),
    };
}

detail::BuiltUrl Client::MakeVirtualHostUrl(String path, String protocol) const
{
    auto bucket_validated = detail::ValidateVirtualHostBucketName(bucket_name_);
    Invariant(bucket_validated, "presign requires non-empty bucket"_t);

    auto raw_path = MakeRawPath(std::move(path), IncludeBucket::kNo);
    auto hostname = text::Format("{}.{}", bucket_name_, endpoint_.hostname);
    auto url = endpoint_.url.WithProtocol(protocol).WithHostname(hostname).WithPort(endpoint_.port);
    url = url.WithPathname(raw_path).Without(Url::StripOptions::kHash | Url::StripOptions::kQuery);
    return {
        .href = url.Href(),
        .host = url.Host(),
        .raw_path = std::move(raw_path),
    };
}

String Client::PresignVirtualHost(
    String method, String path, const std::chrono::system_clock::time_point &expires_at,
    String protocol, std::optional<httpc::Headers> extra_headers
) const
{
    auto now = datetime::Now();
    auto built = MakeVirtualHostUrl(std::move(path), std::move(protocol));

    const SigParams params = MakeSigParams(now);

    httpc::Headers extra = extra_headers.value_or(httpc::Headers{});
    auto prepared = PrepareSignedHeaders(built.host.ToBytes(), extra);
    auto headers_text = *text::StringPairs(prepared);

    return MakePresignedUrl(method, built, now, expires_at, params, headers_text);
}

String Client::PresignPathStyle(
    String method, String path, const std::chrono::system_clock::time_point &expires_at,
    String protocol
) const
{
    auto now = datetime::Now();
    auto built = MakePathStyleUrl(std::move(path), std::move(protocol));
    SigParams params = MakeSigParams(now);
    auto prepared = PrepareSignedHeaders(built.host.ToBytes(), httpc::Headers{});
    auto headers_text = *text::StringPairs(prepared);

    return MakePresignedUrl(method, built, now, expires_at, params, headers_text);
}

String Client::MakePresignedUrl(
    String method, const detail::BuiltUrl &built, const std::chrono::system_clock::time_point &now,
    const std::chrono::system_clock::time_point &expires_at, const SigParams &params,
    const std::vector<std::pair<String, String>> &headers
) const
{
    const std::string scope = MakeScope(params);
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
    query.emplace_back(
        "X-Amz-Credential", std::format("{}/{}", params.access_key_id.GetUnderlying(), scope)
    );
    query.emplace_back("X-Amz-Date", params.amz_date);
    query.emplace_back("X-Amz-Expires", std::to_string(ComputePresignTtl(now, expires_at).count()));

    auto headers_utf8 = text::ToBytesPairs(headers);
    const std::string signed_headers = MakeSignedHeaders(headers_utf8);
    query.emplace_back("X-Amz-SignedHeaders", signed_headers);
    if (params.session_token)
        query.emplace_back("X-Amz-Security-Token", params.session_token->GetUnderlying().ToBytes());

    auto cr = MakeCanonicalRequest(
        method.View(), built.raw_path.View(), query, headers_utf8, "UNSIGNED-PAYLOAD"
    );
    const std::string string_to_sign = std::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", params.amz_date, scope, Sha256Hex(cr.canonical_request)
    );
    const std::string signature = ComputeSignature(params, string_to_sign);
    query.emplace_back("X-Amz-Signature", signature);

    auto url = built.href.ToBytes();
    url.push_back('?');
    url.append(CanonicalizeQuery(query));
    return *String::FromBytes(url);
}

} // namespace ws::s3
