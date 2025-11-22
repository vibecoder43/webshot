#pragma once

#include <chrono>
#include <string>

#include <userver/clients/http/client.hpp>

#include "s3_credentials_types.hpp"

namespace v1 {

/**
 * @brief Result of a single STS AssumeRole call for S3 credentials.
 */
struct [[nodiscard]] StsCredentials {
    s3v4::AccessKeyId accessKeyId;
    s3v4::SecretAccessKey secretAccessKey;
    s3v4::SessionToken sessionToken;
    std::chrono::system_clock::time_point expiresAt;

    explicit StsCredentials(const std::string &xml);
};

/**
 * @brief Call STS AssumeRole at the given endpoint and parse temporary S3
 * credentials.
 *
 * The endpoint must use https. A prebuilt policy JSON is passed verbatim.
 */
[[nodiscard]] StsCredentials FetchStsCredentials(
    userver::clients::http::Client &httpClient, const std::string &stsEndpoint,
    const s3v4::AccessKeyId &staticAccessKeyId, const s3v4::SecretAccessKey &staticSecretAccessKey,
    const std::string &region, const std::string &roleArn, const std::string &roleSessionName,
    const std::string &policyJson, std::chrono::seconds duration, std::chrono::milliseconds timeout
);

} // namespace v1
