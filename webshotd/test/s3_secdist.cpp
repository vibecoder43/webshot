#include <optional>
#include <string>

#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/utest/utest.hpp>

#include "s3_secdist.hpp"

using v1::S3CredentialsSecdist;

UTEST(S3Secdist, ParsesAllFields)
{
    userver::formats::json::ValueBuilder builder;
    auto creds = builder["s3_credentials"];
    creds["access_key_id"] = "ACCESS";
    creds["secret_access_key"] = "SECRET";
    creds["session_token"] = "TOKEN";

    const S3CredentialsSecdist parsed(builder.ExtractValue());
    ASSERT_TRUE(parsed.accessKeyId);
    ASSERT_TRUE(parsed.secretAccessKey);
    ASSERT_TRUE(parsed.sessionToken);
    if (!parsed.accessKeyId || !parsed.secretAccessKey || !parsed.sessionToken)
        return;
    EXPECT_EQ(parsed.accessKeyId->GetUnderlying().view(), std::string("ACCESS"));
    EXPECT_EQ(parsed.secretAccessKey->GetUnderlying().view(), std::string("SECRET"));
    EXPECT_EQ(parsed.sessionToken->GetUnderlying().view(), std::string("TOKEN"));
}

UTEST(S3Secdist, MissingObjectYieldsNullopts)
{
    userver::formats::json::ValueBuilder builder; // empty root
    const S3CredentialsSecdist parsed(builder.ExtractValue());
    EXPECT_FALSE(parsed.accessKeyId);
    EXPECT_FALSE(parsed.secretAccessKey);
    EXPECT_FALSE(parsed.sessionToken);
}

UTEST(S3Secdist, PartialCredentials)
{
    userver::formats::json::ValueBuilder builder;
    auto creds = builder["s3_credentials"];
    creds["access_key_id"] = "ACCESS_ONLY";

    const S3CredentialsSecdist parsed(builder.ExtractValue());
    ASSERT_TRUE(parsed.accessKeyId);
    if (!parsed.accessKeyId)
        return;
    EXPECT_EQ(parsed.accessKeyId->GetUnderlying().view(), std::string("ACCESS_ONLY"));
    EXPECT_FALSE(parsed.secretAccessKey);
    EXPECT_FALSE(parsed.sessionToken);
}
