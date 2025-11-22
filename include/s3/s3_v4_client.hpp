#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <userver/clients/http/client.hpp>
#include <userver/s3api/clients/s3api.hpp>
#include <userver/utils/strong_typedef.hpp>

#include "s3_credentials_types.hpp"

namespace v1::s3v4 {

struct SigV4Params;

namespace detail {
struct EndpointParts {
    std::string scheme;    // http or https
    std::string authority; // host or host:port
};
} // namespace detail

/**
 * @brief Connection parameters for a minimal SigV4 S3 client.
 */
struct [[nodiscard]] S3V4Config {
    std::string endpoint; // e.g. http://localhost:8333 or s3.amazonaws.com
    std::string region;   // e.g. us-east-1, local, etc.
    std::chrono::milliseconds timeout;
    bool virtualHosted = false; // not used in v1; path-style addressing by default
};

/**
 * @brief Credentials used for signing requests with AWS Signature V4.
 */
struct [[nodiscard]] S3Credentials {
    AccessKeyId accessKeyId;
    SecretAccessKey secretAccessKey;
    std::optional<SessionToken> sessionToken; // optional
};

/**
 * @brief Minimal SigV4 S3 client implementation.
 *
 * Only the subset of methods required by this service is implemented.
 */
class [[nodiscard]] S3V4Client final : public userver::s3api::Client {
public:
    S3V4Client(
        userver::clients::http::Client &http, S3V4Config cfg, S3Credentials creds,
        std::string defaultBucket
    );

    std::string PutObject(
        std::string_view path, std::string data, const std::optional<Meta> &meta,
        std::string_view content_type, const std::optional<std::string> &content_disposition,
        const std::optional<std::vector<Tag>> &tags
    ) const override;

    void DeleteObject(std::string_view path) const override;

    std::optional<HeadersDataResponse>
    GetObjectHead(std::string_view path, const HeaderDataRequest &request) const override;

    std::optional<std::string>
    GetObject(std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &)
        const override;
    std::string
    TryGetObject(std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &)
        const override;
    std::optional<std::string>
    GetPartialObject(std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &)
        const override;
    std::string
    TryGetPartialObject(std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *, const HeaderDataRequest &)
        const override;
    std::string
    CopyObject(std::string_view, std::string_view, std::string_view, const std::optional<Meta> &)
        override;
    std::string
    CopyObject(std::string_view, std::string_view, const std::optional<Meta> &) override;
    std::optional<std::string>
    ListBucketContents(std::string_view, int, std::string, std::string) const override;
    std::vector<userver::s3api::ObjectMeta> ListBucketContentsParsed(std::string_view
    ) const override;
    std::vector<std::string> ListBucketDirectories(std::string_view) const override;
    void UpdateConfig(userver::s3api::ConnectionCfg &&) override;
    std::string_view GetBucketName() const override;

    std::string
    GenerateDownloadUrl(std::string_view path, time_t expires_epoch, bool use_ssl) const override;
    std::string GenerateDownloadUrlVirtualHostAddressing(
        std::string_view path, const std::chrono::system_clock::time_point &expires_at,
        std::string_view protocol
    ) const override;
    std::string GenerateUploadUrlVirtualHostAddressing(
        std::string_view data, std::string_view content_type, std::string_view path,
        const std::chrono::system_clock::time_point &expires_at, std::string_view protocol
    ) const override;

private:
    static std::string CanonicalizeQuery(std::vector<std::pair<std::string, std::string>> q);
    static std::string
    ComputePresignSignature(const SigV4Params &params, const std::string &string_to_sign);

    static std::chrono::seconds ComputePresignTtl(
        const std::chrono::system_clock::time_point &now,
        const std::chrono::system_clock::time_point &expires_at
    );
    [[nodiscard]] SigV4Params MakeSigV4Params(const std::chrono::system_clock::time_point &now
    ) const;
    void SignRequest(
        std::string_view method, const std::string &req, userver::clients::http::Headers &headers,
        const std::string &payload_hash
    ) const;
    std::string MakeReq(std::string_view path) const;
    std::string PresignVirtualHost(
        std::string_view method, std::string_view path,
        const std::chrono::system_clock::time_point &expires_at, std::string_view protocol,
        std::optional<userver::clients::http::Headers> extra_headers
    ) const;
    std::string PresignPathStyle(
        std::string_view method, std::string_view path,
        const std::chrono::system_clock::time_point &expires_at, std::string_view protocol
    ) const;

    userver::clients::http::Client &http_;
    S3V4Config cfg_;
    S3Credentials creds_;
    std::string bucket_;
    detail::EndpointParts ep_;
};

} // namespace v1::s3v4
