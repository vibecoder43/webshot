#include <chrono>
#include <string>

#include <userver/clients/http/response.hpp>
#include <userver/utest/utest.hpp>

#include "s3/s3_sts_client.hpp"
#include "text.hpp"

using v1::StsCredentials;
using namespace text::literals;

namespace {

std::string makeValidXml()
{
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<AssumeRoleResponse>
  <AssumeRoleResult>
    <Credentials>
      <AccessKeyId>AKIA_TEST_KEY</AccessKeyId>
      <SecretAccessKey>SECRET_TEST_KEY</SecretAccessKey>
      <SessionToken>TOKEN_TEST_VALUE</SessionToken>
      <Expiration>2025-12-01T00:00:00Z</Expiration>
    </Credentials>
  </AssumeRoleResult>
</AssumeRoleResponse>)";
}

} // namespace

UTEST(S3StsClient, ParsesHappyPathXml)
{
    const StsCredentials creds(String::fromBytesThrow(makeValidXml()));

    EXPECT_EQ(creds.accessKeyId.GetUnderlying().view(), std::string("AKIA_TEST_KEY"));
    EXPECT_EQ(creds.secretAccessKey.GetUnderlying().view(), std::string("SECRET_TEST_KEY"));
    EXPECT_EQ(creds.sessionToken.GetUnderlying().view(), std::string("TOKEN_TEST_VALUE"));

    using std::chrono::system_clock;
    const auto expected = system_clock::from_time_t(1764547200); // 2025-12-01T00:00:00Z
    EXPECT_EQ(system_clock::to_time_t(creds.expiresAt), system_clock::to_time_t(expected));
}

UTEST(S3StsClient, MissingTagThrows)
{
    const std::string xml =
        R"(<AssumeRoleResponse><Credentials></Credentials></AssumeRoleResponse>)";
    EXPECT_THROW(StsCredentials creds(String::fromBytesThrow(xml)), std::runtime_error);
}

UTEST(S3StsClient, MissingClosingTagThrows)
{
    const std::string xml = R"(<AssumeRoleResponse><AssumeRoleResult><Credentials><AccessKeyId>id)";
    EXPECT_THROW(StsCredentials creds(String::fromBytesThrow(xml)), std::runtime_error);
}

UTEST(S3StsClient, BuildsRequestWithExecutor)
{
    std::string capturedUrl;
    std::string capturedBody;
    userver::clients::http::Headers capturedHeaders;
    std::chrono::milliseconds capturedTimeout{0};

    v1::detail::StsExecutor exec = [&](const String &url, const String &body,
                                       const userver::clients::http::Headers &headers,
                                       std::chrono::milliseconds timeout) {
        capturedUrl = std::string(url.view());
        capturedBody = std::string(body.view());
        capturedHeaders = headers;
        capturedTimeout = timeout;
        return makeValidXml();
    };

    const std::string endpoint = "https://sts.example.com/assume?foo=bar";
    const auto creds = v1::detail::fetchStsWithExecutor(
        exec, String::fromBytesThrow(endpoint), v1::s3v4::AccessKeyId{"AKIA_STATIC"_t},
        v1::s3v4::SecretAccessKey{"SECRET"_t}, "us-east-1"_t,
        "arn:aws:iam::123456789012:role/TestRole"_t, "session-name"_t, R"({"allow":true})"_t,
        std::chrono::seconds{900}, std::chrono::milliseconds{1500}
    );

    EXPECT_EQ(capturedUrl, endpoint);
    const std::string authKey = "authorization";
    const std::string amzDateKey = "x-amz-date";
    const std::string contentTypeKey = "content-type";
    const std::string hostKey = "host";
    EXPECT_NE(capturedHeaders.find(authKey), capturedHeaders.end());
    EXPECT_NE(capturedHeaders.find(amzDateKey), capturedHeaders.end());
    EXPECT_EQ(capturedHeaders.at(contentTypeKey), "application/x-www-form-urlencoded");
    EXPECT_EQ(capturedHeaders.at(hostKey), std::string("sts.example.com"));
    EXPECT_EQ(capturedTimeout, std::chrono::milliseconds{1500});

    EXPECT_NE(capturedBody.find("Action=AssumeRole"), std::string::npos);
    EXPECT_NE(capturedBody.find("Version=2011-06-15"), std::string::npos);
    EXPECT_NE(
        capturedBody.find("RoleArn=arn%3Aaws%3Aiam%3A%3A123456789012%3Arole%2FTestRole"),
        std::string::npos
    );
    EXPECT_NE(capturedBody.find("RoleSessionName=session-name"), std::string::npos);
    EXPECT_NE(capturedBody.find("DurationSeconds=900"), std::string::npos);
    EXPECT_NE(capturedBody.find("Policy=%7B%22allow%22%3Atrue%7D"), std::string::npos);

    EXPECT_EQ(creds.accessKeyId.GetUnderlying().view(), std::string("AKIA_TEST_KEY"));
}
