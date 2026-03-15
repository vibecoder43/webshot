#pragma once

#include "text.hpp"

#include <optional>
#include <string>

#include <userver/formats/json/value.hpp>
#include <userver/utils/assert.hpp>

#include "s3_credentials_types.hpp"

namespace v1 {

/**
 * @brief Light wrapper to read S3 credentials from secdist.
 *
 * This type looks for the `s3_credentials` object with `access_key_id` and
 * `secret_access_key` fields and exposes them as optionals. An optional
 * `session_token` is also supported for temporary credentials.
 */
struct S3CredentialsSecdist {
    std::optional<s3v4::AccessKeyId> accessKeyId;
    std::optional<s3v4::SecretAccessKey> secretAccessKey;
    std::optional<s3v4::SessionToken> sessionToken;

    explicit S3CredentialsSecdist(const userver::formats::json::Value &secdistDoc)
    {
        const auto creds = secdistDoc["s3_credentials"];
        if (!creds.IsMissing()) {
            if (auto v = creds["access_key_id"]; !v.IsMissing())
                accessKeyId = s3v4::AccessKeyId(String::fromBytesThrow(v.As<std::string>()));
            if (auto v = creds["secret_access_key"]; !v.IsMissing())
                secretAccessKey = s3v4::SecretAccessKey(
                    String::fromBytesThrow(v.As<std::string>())
                );
            if (auto v = creds["session_token"]; !v.IsMissing())
                sessionToken = s3v4::SessionToken(String::fromBytesThrow(v.As<std::string>()));
        }
    }
};

} // namespace v1
