#pragma once

#include "expected.hpp"
#include "s3_credentials_types.hpp"
#include "text.hpp"
#include "url.hpp"
#include "userver_namespaces.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <userver/clients/http/client.hpp>
#include <userver/s3api/clients/s3api.hpp>
#include <userver/utils/strong_typedef.hpp>

namespace v1::s3v4 {

struct SigV4Params;

namespace detail {
struct EndpointParts {
    Url url;
    String host;     // host header, may include port
    String hostname; // host without port
    String port;     // optional, empty if not set
    String basePath; // leading slash, no trailing slash unless root
};

struct BuiltUrl {
    String href;    // fully encoded absolute URL without query
    String host;    // host header value
    String rawPath; // unencoded path used for canonicalization (starts with '/')
};

enum class VirtualHostPresignError {
    kMissingBucket,
};

[[nodiscard]] Expected<void, VirtualHostPresignError>
validateVirtualHostBucketName(const String &bucketName);
} // namespace detail

/**
 * @brief Connection parameters for a minimal SigV4 S3 client.
 */
struct [[nodiscard]] S3V4Config {
    String endpoint; // e.g. http://localhost:8333 or s3.amazonaws.com
    String region;   // e.g. us-east-1, local, etc.
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
 * Only the subset of methods required by this service is implemented; newly
 * added interface methods that are not used by this service are provided as
 * stubs that abort if called.
 */
class [[nodiscard]] S3V4Client final : public us::s3api::Client {
public:
    S3V4Client(
        httpc::Client &httpClient, S3V4Config config, S3Credentials creds, String bucketName
    );

    std::string PutObject(
        std::string_view path, std::string data, const std::optional<Meta> &meta,
        std::string_view contentType, const std::optional<std::string> &contentDisposition,
        const std::optional<std::vector<Tag>> &tags
    ) const override;

    void DeleteObject(std::string_view path) const override;

    std::optional<HeadersDataResponse>
    GetObjectHead(std::string_view path, const HeaderDataRequest &request) const override;

    std::optional<std::string> GetObject(
        std::string_view, std::optional<std::string>, HeadersDataResponse *,
        const HeaderDataRequest &
    ) const override;
    std::string TryGetObject(
        std::string_view, std::optional<std::string>, HeadersDataResponse *,
        const HeaderDataRequest &
    ) const override;
    std::optional<std::string> GetPartialObject(
        std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *,
        const HeaderDataRequest &
    ) const override;
    std::string TryGetPartialObject(
        std::string_view, std::string_view, std::optional<std::string>, HeadersDataResponse *,
        const HeaderDataRequest &
    ) const override;
    std::string CopyObject(
        std::string_view, std::string_view, std::string_view, const std::optional<Meta> &
    ) override;
    std::string
    CopyObject(std::string_view, std::string_view, const std::optional<Meta> &) override;
    std::optional<std::string>
    ListBucketContents(std::string_view, int, std::string, std::string) const override;
    std::vector<us::s3api::ObjectMeta> ListBucketContentsParsed(std::string_view) const override;
    std::vector<std::string> ListBucketDirectories(std::string_view) const override;
    void UpdateConfig(us::s3api::ConnectionCfg &&) override;
    std::string_view GetBucketName() const override;

    // Multipart upload APIs are not used by this service; they are implemented
    // as stubs to satisfy the us::s3api::Client interface.
    us::s3api::multipart_upload::InitiateMultipartUploadResult CreateMultipartUpload(
        const us::s3api::multipart_upload::CreateMultipartUploadRequest &request
    ) const override;
    us::s3api::multipart_upload::UploadPartResult
    UploadPart(const us::s3api::multipart_upload::UploadPartRequest &request) const override;
    us::s3api::multipart_upload::CompleteMultipartUploadResult CompleteMultipartUpload(
        const us::s3api::multipart_upload::CompleteMultipartUploadRequest &request
    ) const override;
    void AbortMultipartUpload(
        const us::s3api::multipart_upload::AbortMultipartUploadRequest &request
    ) const override;
    us::s3api::multipart_upload::ListPartsResult
    ListParts(const us::s3api::multipart_upload::ListPartsRequest &request) const override;
    us::s3api::multipart_upload::ListMultipartUploadsResult ListMultipartUploads(
        const us::s3api::multipart_upload::ListMultipartUploadsRequest &request
    ) const override;

    std::string
    GenerateDownloadUrl(std::string_view path, time_t expiresEpoch, bool useSsl) const override;
    std::string GenerateDownloadUrlVirtualHostAddressing(
        std::string_view path, const std::chrono::system_clock::time_point &expiresAt,
        std::string_view protocol
    ) const override;
    std::string GenerateUploadUrlVirtualHostAddressing(
        std::string_view data, std::string_view contentType, std::string_view path,
        const std::chrono::system_clock::time_point &expiresAt, std::string_view protocol
    ) const override;

private:
    enum class IncludeBucket {
        kNo,
        kYes,
    };

    static std::chrono::seconds computePresignTtl(
        const std::chrono::system_clock::time_point &now,
        const std::chrono::system_clock::time_point &expiresAt
    );
    [[nodiscard]] SigV4Params
    makeSigV4Params(const std::chrono::system_clock::time_point &now) const;
    void signRequest(
        String method, String canonicalUri, String host, httpc::Headers &headers,
        const String &payloadHash
    ) const;
    [[nodiscard]] detail::BuiltUrl
    makePathStyleUrl(String path, std::optional<String> protocolOverride) const;
    [[nodiscard]] detail::BuiltUrl makeVirtualHostUrl(String path, String protocol) const;
    [[nodiscard]] String buildRawPath(String path, IncludeBucket includeBucket) const;
    String presignVirtualHost(
        String method, String path, const std::chrono::system_clock::time_point &expiresAt,
        String protocol, std::optional<httpc::Headers> extraHeaders
    ) const;
    String presignPathStyle(
        String method, String path, const std::chrono::system_clock::time_point &expiresAt,
        String protocol
    ) const;
    String buildPresignedUrl(
        String method, const detail::BuiltUrl &built,
        const std::chrono::system_clock::time_point &now,
        const std::chrono::system_clock::time_point &expiresAt, const SigV4Params &params,
        const std::vector<std::pair<String, String>> &headers
    ) const;

    httpc::Client &httpClient;
    S3V4Config config;
    S3Credentials creds;
    String bucketName;
    detail::EndpointParts endpoint;
};

} // namespace v1::s3v4
