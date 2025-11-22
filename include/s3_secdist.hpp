#pragma once

#include <optional>
#include <string>

#include <userver/formats/json/value.hpp>

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
    std::optional<s3v4::AccessKeyId> access_key_id;
    std::optional<s3v4::SecretAccessKey> secret_access_key;
    std::optional<s3v4::SessionToken> session_token;

    explicit S3CredentialsSecdist(const userver::formats::json::Value &secdist_doc)
    {
        const auto creds = secdist_doc["s3_credentials"];
        if (!creds.IsMissing()) {
            if (auto v = creds["access_key_id"]; !v.IsMissing())
                access_key_id = s3v4::AccessKeyId{v.As<std::string>()};
            if (auto v = creds["secret_access_key"]; !v.IsMissing())
                secret_access_key = s3v4::SecretAccessKey{v.As<std::string>()};
            if (auto v = creds["session_token"]; !v.IsMissing())
                session_token = s3v4::SessionToken{v.As<std::string>()};
        }
    }
};

} // namespace v1
