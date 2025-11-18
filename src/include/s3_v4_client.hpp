#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <userver/clients/http/client.hpp>
#include <userver/s3api/clients/s3api.hpp>

namespace v1::s3v4 {

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
    std::string accessKeyId;
    std::string secretAccessKey;
    std::optional<std::string> sessionToken; // optional
};

/**
 * @brief Create a userver S3 client that signs requests using SigV4.
 *
 * Only the subset of S3 methods required by this service is implemented.
 *
 * @param http Shared HTTP client.
 * @param cfg Endpoint, region, timeouts, addressing mode.
 * @param creds Access keys for signing.
 * @param defaultBucket Optional default bucket for path‑style addressing.
 */
[[nodiscard]] userver::s3api::ClientPtr MakeS3ClientV4(
    userver::clients::http::Client &http, S3V4Config cfg, S3Credentials creds,
    std::string defaultBucket
);

} // namespace v1::s3v4
