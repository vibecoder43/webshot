#pragma once

#include <optional>
#include <string>

#include <userver/formats/json/value.hpp>

namespace v1 {

struct S3CredentialsSecdist {
    std::optional<std::string> access_key_id;
    std::optional<std::string> secret_access_key;

    explicit S3CredentialsSecdist(const userver::formats::json::Value &secdist_doc)
    {
        const auto creds = secdist_doc["s3_credentials"];
        if (!creds.IsMissing()) {
            if (auto v = creds["access_key_id"]; !v.IsMissing())
                access_key_id = v.As<std::string>();
            if (auto v = creds["secret_access_key"]; !v.IsMissing())
                secret_access_key = v.As<std::string>();
        }
    }
};

} // namespace v1
