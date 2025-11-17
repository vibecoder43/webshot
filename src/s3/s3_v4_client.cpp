/**
 * @file
 * @brief Minimal S3 client that signs requests with AWS Signature V4.
 *
 * Implements the subset of methods required by this service (PUT, HEAD, DELETE
 * and presign helpers). It relies on userver HTTP client for transport.
 */
#include "../include/s3_v4_client.hpp"
#include "sigv4_signer.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include <userver/crypto/hash.hpp>

#include <userver/clients/http/client.hpp>
#include <userver/clients/http/response.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/logging/log.hpp>

namespace s3 = userver::s3api;
namespace http = userver::clients::http;

namespace v1::s3v4 {

namespace {

/** Simple endpoint parser: returns {scheme, host[:port]}. */
struct EndpointParts {
    std::string scheme;    // http or https
    std::string authority; // host or host:port
};

EndpointParts ParseEndpoint(std::string ep)
{
    EndpointParts out;
    auto pos = ep.find("://");
    if (pos != std::string::npos) {
        out.scheme = ep.substr(0, pos);
        out.authority = ep.substr(pos + 3);
    } else {
        out.scheme = "https";
        out.authority = std::move(ep);
    }
    // strip trailing slash if present
    while (!out.authority.empty() && out.authority.back() == '/')
        out.authority.pop_back();
    return out;
}

/** Build full URL for path‑style addressing: scheme://authority/req. */
std::string BuildUrl(const EndpointParts &ep, const std::string &req)
{
    std::string url;
    url.reserve(ep.scheme.size() + 3 + ep.authority.size() + 1 + req.size());
    url.append(ep.scheme).append("://").append(ep.authority);
    if (!req.empty()) {
        url.push_back('/');
        url.append(req);
    }
    return url;
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

class S3V4ClientImpl final : public s3::Client {
public:
    S3V4ClientImpl(
        http::Client &http, S3V4Config cfg, S3Credentials creds, std::string defaultBucket
    )
        : http_(http), cfg_(std::move(cfg)), creds_(std::move(creds)),
          bucket_(std::move(defaultBucket)), ep_(ParseEndpoint(cfg_.endpoint))
    {
    }

    // methods used by this service
    std::string PutObject(
        std::string_view path, std::string data, const std::optional<Meta> &meta,
        std::string_view content_type, const std::optional<std::string> &content_disposition,
        const std::optional<std::vector<Tag>> &tags
    ) const override
    {
        (void)tags; // not used in current service
        const std::string req = MakeReq(path);
        http::Headers headers;
        headers[userver::http::headers::kContentType] = std::string(content_type);
        if (content_disposition)
            headers[userver::http::headers::kContentDisposition] = *content_disposition;
        if (meta) {
            for (const auto &kv : *meta)
                headers["x-amz-meta-" + kv.first] = kv.second;
        }
        const std::string payload_hash = Sha256Hex(data);
        SignRequest("PUT", req, headers, payload_hash);

        const std::string url = BuildUrl(ep_, req);
        auto resp = http_.CreateNotSignedRequest()
                        .put(url, std::move(data))
                        .headers(headers)
                        .timeout(cfg_.timeout)
                        .perform();
        resp->raise_for_status();
        return resp->body();
    }

    void DeleteObject(std::string_view path) const override
    {
        const std::string req = MakeReq(path);
        http::Headers headers;
        const std::string payload_hash = Sha256Hex("");
        SignRequest("DELETE", req, headers, payload_hash);

        const std::string url = BuildUrl(ep_, req);
        auto resp = http_.CreateNotSignedRequest()
                        .delete_method(url)
                        .headers(headers)
                        .timeout(cfg_.timeout)
                        .perform();
        resp->raise_for_status();
    }

    // Minimal header HEAD support (used indirectly by copy in userver, not used here)
    std::optional<HeadersDataResponse>
    GetObjectHead(std::string_view path, const HeaderDataRequest &request) const override
    {
        const std::string req = MakeReq(path);
        http::Headers headers;
        const std::string payload_hash = Sha256Hex("");
        SignRequest("HEAD", req, headers, payload_hash);
        const std::string url = BuildUrl(ep_, req);
        auto resp = http_.CreateNotSignedRequest()
                        .head(url)
                        .headers(headers)
                        .timeout(cfg_.timeout)
                        .perform();
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
    std::optional<std::string>
    GetObject(std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &)
        const override
    {
        throw std::runtime_error("GetObject not implemented in SigV4 client for this service");
    }
    std::string
    TryGetObject(std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &)
        const override
    {
        throw std::runtime_error("TryGetObject not implemented in SigV4 client");
    }
    std::optional<std::string>
    GetPartialObject(std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &)
        const override
    {
        throw std::runtime_error("GetPartialObject not implemented in SigV4 client");
    }
    std::string
    TryGetPartialObject(std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &)
        const override
    {
        throw std::runtime_error("TryGetPartialObject not implemented in SigV4 client");
    }
    std::string
    CopyObject(std::string_view, std::string_view, std::string_view, const std::optional<Meta> &)
        override
    {
        throw std::runtime_error("CopyObject not implemented in SigV4 client");
    }
    std::string CopyObject(std::string_view, std::string_view, const std::optional<Meta> &) override
    {
        throw std::runtime_error("CopyObject not implemented in SigV4 client");
    }
    std::optional<std::string>
    ListBucketContents(std::string_view, int, std::string, std::string) const override
    {
        throw std::runtime_error("ListBucketContents not implemented in SigV4 client");
    }
    std::vector<s3::ObjectMeta> ListBucketContentsParsed(std::string_view) const override
    {
        throw std::runtime_error("ListBucketContentsParsed not implemented in SigV4 client");
    }
    std::vector<std::string> ListBucketDirectories(std::string_view) const override
    {
        throw std::runtime_error("ListBucketDirectories not implemented in SigV4 client");
    }
    void UpdateConfig(s3::ConnectionCfg &&) override {}
    std::string_view GetBucketName() const override { return bucket_; }

    // Presign support using chrono
    std::string
    GenerateDownloadUrl(std::string_view path, time_t expires_epoch, bool use_ssl) const override
    {
        const auto expires_at = std::chrono::system_clock::from_time_t(expires_epoch);
        const auto protocol = use_ssl ? std::string{"https"} : std::string{"http"};
        return PresignPathStyle("GET", path, expires_at, protocol);
    }
    std::string GenerateDownloadUrlVirtualHostAddressing(
        std::string_view path, const std::chrono::system_clock::time_point &expires_at,
        std::string_view protocol
    ) const override
    {
        if (bucket_.empty())
            throw std::runtime_error("presign requires non-empty bucket");
        return PresignVirtualHost("GET", path, expires_at, protocol, std::nullopt);
    }
    std::string GenerateUploadUrlVirtualHostAddressing(
        std::string_view data, std::string_view content_type, std::string_view path,
        const std::chrono::system_clock::time_point &expires_at, std::string_view protocol
    ) const override
    {
        if (bucket_.empty())
            throw std::runtime_error("presign requires non-empty bucket");
        http::Headers hdrs;
        hdrs[userver::http::headers::kContentType] = std::string(content_type);
        (void)data; // we use UNSIGNED-PAYLOAD for presign
        return PresignVirtualHost("PUT", path, expires_at, protocol, std::move(hdrs));
    }

private:
    static std::chrono::seconds ComputePresignTtl(
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

    [[nodiscard]] SigV4Params MakeSigV4Params(const std::chrono::system_clock::time_point &now
    ) const
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

    void SignRequest(
        std::string_view method, const std::string &req, http::Headers &headers,
        const std::string &payload_hash
    ) const
    {
        const auto now = std::chrono::system_clock::now();
        const SigV4Params params = MakeSigV4Params(now);

        auto to_sign = PrepareSignedHeaders(ep_.authority, headers);
        auto signed_map = SignHeaders(
            params, method, "/" + req, /*query*/ {}, to_sign, payload_hash
        );
        for (const auto &kv : signed_map)
            headers[kv.first] = kv.second;
    }

    std::string MakeReq(std::string_view path) const
    {
        // If constructed with default bucket, path is key only; otherwise service passes
        // bucket/key.
        if (!bucket_.empty()) {
            std::string req;
            req.reserve(bucket_.size() + 1 + path.size());
            req.append(bucket_).push_back('/');
            req.append(path);
            return req;
        }
        return std::string(path);
    }

    static std::string CanonicalizeQuery(std::vector<std::pair<std::string, std::string>> q)
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
        for (std::size_t i = 0; i < q.size(); ++i) {
            if (i)
                oss << '&';
            oss << q[i].first << '=' << q[i].second;
        }
        return oss.str();
    }

    std::string PresignVirtualHost(
        std::string_view method, std::string_view path,
        const std::chrono::system_clock::time_point &expires_at, std::string_view protocol,
        std::optional<http::Headers> extra_headers
    ) const
    {
        const auto now = std::chrono::system_clock::now();
        const auto ttl = ComputePresignTtl(now, expires_at);

        const std::string host = fmt::format("{}.{}", bucket_, ep_.authority);
        const std::string req = std::string(path);

        const SigV4Params params = MakeSigV4Params(now);

        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("host", host);
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
        query.emplace_back("X-Amz-Credential", fmt::format("{}/{}", params.accessKeyId, scope));
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
            query.emplace_back("X-Amz-Security-Token", *params.sessionToken);

        const auto cr = BuildCanonicalRequest(
            method, std::string("/").append(req), query, headers, std::string("UNSIGNED-PAYLOAD")
        );
        const std::string string_to_sign = fmt::format(
            "AWS4-HMAC-SHA256\n{}\n{}\n{}", params.amzDate, scope, Sha256Hex(cr.canonicalRequest)
        );
        const std::string signature = ComputePresignSignature(params, string_to_sign);
        query.emplace_back("X-Amz-Signature", signature);

        std::string url;
        url.reserve(16 + host.size() + req.size() + 100);
        url.append(protocol).append("://").append(host).push_back('/');
        url.append(req).push_back('?');
        url.append(CanonicalizeQuery(std::move(query)));
        return url;
    }

    std::string PresignPathStyle(
        std::string_view method, std::string_view path,
        const std::chrono::system_clock::time_point &expires_at, std::string_view protocol
    ) const
    {
        const auto now = std::chrono::system_clock::now();
        const auto ttl = ComputePresignTtl(now, expires_at);

        const std::string host = ep_.authority;
        const std::string req = MakeReq(path);

        const SigV4Params params = MakeSigV4Params(now);

        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("host", host);
        std::sort(headers.begin(), headers.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });

        const std::string scope = fmt::format(
            "{}/{}/{}/aws4_request", params.date, params.region, params.service
        );
        std::vector<std::pair<std::string, std::string>> query;
        query.emplace_back("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
        query.emplace_back("X-Amz-Credential", fmt::format("{}/{}", params.accessKeyId, scope));
        query.emplace_back("X-Amz-Date", params.amzDate);
        query.emplace_back("X-Amz-Expires", std::to_string(ttl.count()));
        query.emplace_back("X-Amz-SignedHeaders", std::string("host"));
        if (params.sessionToken)
            query.emplace_back("X-Amz-Security-Token", *params.sessionToken);

        const auto cr = BuildCanonicalRequest(
            method, std::string("/").append(req), query, headers, std::string("UNSIGNED-PAYLOAD")
        );
        const std::string string_to_sign = fmt::format(
            "AWS4-HMAC-SHA256\n{}\n{}\n{}", params.amzDate, scope, Sha256Hex(cr.canonicalRequest)
        );
        const std::string signature = ComputePresignSignature(params, string_to_sign);
        query.emplace_back("X-Amz-Signature", signature);

        std::string url;
        url.reserve(16 + host.size() + req.size() + 100);
        url.append(protocol).append("://").append(host).push_back('/');
        url.append(req).push_back('?');
        url.append(CanonicalizeQuery(std::move(query)));
        return url;
    }

    static std::string
    ComputePresignSignature(const SigV4Params &params, const std::string &string_to_sign)
    {
        namespace US = USERVER_NAMESPACE::crypto::hash;
        const std::string kSecret = fmt::format("AWS4{}", params.secretAccessKey);
        const std::string kDate = US::HmacSha256(kSecret, params.date, US::OutputEncoding::kBinary);
        const std::string kRegion = US::HmacSha256(
            kDate, params.region, US::OutputEncoding::kBinary
        );
        const std::string kService = US::HmacSha256(
            kRegion, params.service, US::OutputEncoding::kBinary
        );
        const std::string kSigning = US::HmacSha256(
            kService, "aws4_request", US::OutputEncoding::kBinary
        );
        return US::HmacSha256(kSigning, string_to_sign, US::OutputEncoding::kHex);
    }

    http::Client &http_;
    S3V4Config cfg_;
    S3Credentials creds_;
    std::string bucket_;
    EndpointParts ep_;
};

} // namespace

userver::s3api::ClientPtr MakeS3ClientV4(
    userver::clients::http::Client &http, S3V4Config cfg, S3Credentials creds,
    std::string defaultBucket
)
{
    return std::static_pointer_cast<s3::Client>(std::make_shared<S3V4ClientImpl>(
        http, std::move(cfg), std::move(creds), std::move(defaultBucket)
    ));
}

} // namespace v1::s3v4
