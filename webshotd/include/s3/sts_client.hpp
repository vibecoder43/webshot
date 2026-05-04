#pragma once

#include "expected.hpp"
#include "s3/credentials_types.hpp"
#include "text.hpp"

#include <chrono>
#include <functional>
#include <string>

#include <userver/clients/http/client.hpp>

namespace ws {

namespace us = userver;
namespace httpc = us::clients::http;
enum class StsError {
    kInvalidEndpoint,
    kInvalidQuery,
    kHttpFailure,
    kXmlMissingTag,
    kXmlMissingClosingTag,
    kInvalidExpiration,
    kInvalidUtf8,
};

/**
 * @brief Result of a single STS AssumeRole call for  credentials.
 */
struct [[nodiscard]] StsCredentials {
    s3::AccessKeyId access_key_id;
    s3::SecretAccessKey secret_access_key;
    s3::SessionToken session_token;
    std::chrono::system_clock::time_point expires_at;

    [[nodiscard]] static Expected<StsCredentials, StsError> FromXml(const String &xml);
};

/**
 * @brief Call STS AssumeRole at the given endpoint and parse temporary
 * credentials.
 *
 * The endpoint must use https. A prebuilt policy JSON is passed verbatim.
 */
[[nodiscard]] Expected<StsCredentials, StsError> FetchStsCredentials(
    httpc::Client &http_client, const String &sts_endpoint,
    const s3::AccessKeyId &static_access_key_id,
    const s3::SecretAccessKey &static_secret_access_key, const String &region,
    const String &role_arn, const String &role_session_name, const String &policy_json,
    std::chrono::seconds duration, std::chrono::milliseconds timeout
);

namespace detail {

using StsExecutor = std::function<Expected<std::string, StsError>(
    const String &url, const String &body, const httpc::Headers &headers,
    std::chrono::milliseconds timeout
)>;

[[nodiscard]] Expected<StsCredentials, StsError> FetchStsWithExecutor(
    const StsExecutor &exec, const String &sts_endpoint,
    const s3::AccessKeyId &static_access_key_id,
    const s3::SecretAccessKey &static_secret_access_key, const String &region,
    const String &role_arn, const String &role_session_name, const String &policy_json,
    std::chrono::seconds duration, std::chrono::milliseconds timeout
);

} // namespace detail

} // namespace ws
