#pragma once

#include "expected.hpp"
#include "s3/credentials_types.hpp"
#include "text.hpp"
#include "url.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <userver/clients/http/client.hpp>
#include <userver/s3api/clients/s3api.hpp>
#include <userver/utils/strong_typedef.hpp>

namespace ws::s3 {

namespace us = userver;
namespace httpc = us::clients::http;
struct SigParams;

namespace detail {
struct EndpointParts {
    Url url;
    String host;      // host header, may include port
    String hostname;  // host without port
    String port;      // optional, empty if not set
    String base_path; // leading slash, no trailing slash unless root
};

struct BuiltUrl {
    String href;     // fully encoded absolute URL without query
    String host;     // host header value
    String raw_path; // unencoded path used for canonicalization (starts with '/')
};

enum class VirtualHostPresignError {
    kMissingBucket,
};

[[nodiscard]] Expected<void, VirtualHostPresignError>
ValidateVirtualHostBucketName(const String &bucket_name);
} // namespace detail

/**
 * @brief Connection parameters for a minimal Sig  client.
 */
struct [[nodiscard]] Config {
    String endpoint; // e.g. http://127.0.0.1:8333 or s3.amazonaws.com
    String region;   // e.g. us-east-1, local, etc.
    std::chrono::milliseconds timeout;
    bool virtual_hosted = false; // not used in ws; path-style addressing by default
};

/**
 * @brief Credentials used for signing requests with AWS Signature .
 */
struct [[nodiscard]] Credentials {
    AccessKeyId access_key_id;
    SecretAccessKey secret_access_key;
    std::optional<SessionToken> session_token; // optional
};

/**
 * @brief Minimal Sig  client implementation.
 *
 * Only the subset of methods required by this service is implemented; newly
 * added interface methods that are not used by this service are provided as
 * stubs that abort if called.
 */
class [[nodiscard]] Client final : public us::s3api::Client {
public:
    Client(httpc::Client &http_client, Config config, Credentials creds, String bucket_name);

    std::string PutObject(
        std::string_view path, std::string data, const std::optional<Meta> &meta,
        std::string_view content_type, const std::optional<std::string> &content_disposition,
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
    enum class IncludeBucket {
        kNo,
        kYes,
    };

    static std::chrono::seconds ComputePresignTtl(
        const std::chrono::system_clock::time_point &now,
        const std::chrono::system_clock::time_point &expires_at
    );
    [[nodiscard]] SigParams MakeSigParams(const std::chrono::system_clock::time_point &now) const;
    void SignRequest(
        String method, String canonical_uri, String host, httpc::Headers &headers,
        const String &payload_hash
    ) const;
    [[nodiscard]] detail::BuiltUrl
    MakePathStyleUrl(String path, std::optional<String> protocol_override) const;
    [[nodiscard]] detail::BuiltUrl MakeVirtualHostUrl(String path, String protocol) const;
    [[nodiscard]] String BuildRawPath(String path, IncludeBucket include_bucket) const;
    String PresignVirtualHost(
        String method, String path, const std::chrono::system_clock::time_point &expires_at,
        String protocol, std::optional<httpc::Headers> extra_headers
    ) const;
    String PresignPathStyle(
        String method, String path, const std::chrono::system_clock::time_point &expires_at,
        String protocol
    ) const;
    String BuildPresignedUrl(
        String method, const detail::BuiltUrl &built,
        const std::chrono::system_clock::time_point &now,
        const std::chrono::system_clock::time_point &expires_at, const SigParams &params,
        const std::vector<std::pair<String, String>> &headers
    ) const;

    httpc::Client &http_client_;
    Config config_;
    Credentials creds_;
    String bucket_name_;
    detail::EndpointParts endpoint_;
};

} // namespace ws::s3
