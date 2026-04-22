#include <chrono>
#include <string>

#include "userver_namespaces.hpp"

#include <userver/clients/http/response.hpp>
#include <userver/utest/utest.hpp>

#include "s3/s3_sts_client.hpp"
#include "text.hpp"

using namespace std::chrono_literals;
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
    const auto parsed = StsCredentials::fromXml(String::fromBytes(makeValidXml()).expect());
    ASSERT_TRUE(parsed);
    const auto &creds = *parsed;

    EXPECT_EQ(creds.accessKeyId.GetUnderlying(), "AKIA_TEST_KEY"_t);
    EXPECT_EQ(creds.secretAccessKey.GetUnderlying(), "SECRET_TEST_KEY"_t);
    EXPECT_EQ(creds.sessionToken.GetUnderlying(), "TOKEN_TEST_VALUE"_t);

    using std::chrono::system_clock;
    const auto expected = system_clock::from_time_t(1764547200); // 2025-12-01T00:00:00Z
    EXPECT_EQ(system_clock::to_time_t(creds.expiresAt), system_clock::to_time_t(expected));
}

UTEST(S3StsClient, MissingTagThrows)
{
    const std::string xml =
        R"(<AssumeRoleResponse><Credentials></Credentials></AssumeRoleResponse>)";
    const auto parsed = StsCredentials::fromXml(String::fromBytes(xml).expect());
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error(), v1::StsError::kXmlMissingTag);
}

UTEST(S3StsClient, MissingClosingTagThrows)
{
    const std::string xml = R"(<AssumeRoleResponse><AssumeRoleResult><Credentials><AccessKeyId>id)";
    const auto parsed = StsCredentials::fromXml(String::fromBytes(xml).expect());
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error(), v1::StsError::kXmlMissingClosingTag);
}

UTEST(S3StsClient, InvalidExpirationReturnsError)
{
    const std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8"?>
<AssumeRoleResponse>
  <AssumeRoleResult>
    <Credentials>
      <AccessKeyId>AKIA_TEST_KEY</AccessKeyId>
      <SecretAccessKey>SECRET_TEST_KEY</SecretAccessKey>
      <SessionToken>TOKEN_TEST_VALUE</SessionToken>
      <Expiration>not-a-timestamp</Expiration>
    </Credentials>
  </AssumeRoleResult>
</AssumeRoleResponse>)";
    const auto parsed = StsCredentials::fromXml(String::fromBytes(xml).expect());
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error(), v1::StsError::kInvalidExpiration);
}

UTEST(S3StsClient, BuildsRequestWithExecutor)
{
    std::string capturedUrl;
    std::string capturedBody;
    httpc::Headers capturedHeaders;
    auto capturedTimeout = 0ms;

    v1::detail::StsExecutor exec = [&](const String &url, const String &body,
                                       const httpc::Headers &headers,
                                       std::chrono::milliseconds timeout) {
        capturedUrl = std::to_string(url);
        capturedBody = std::to_string(body);
        capturedHeaders = headers;
        capturedTimeout = timeout;
        return makeValidXml();
    };

    const std::string endpoint = "https://sts.example.com/assume?foo=bar";
    const auto parsed = v1::detail::fetchStsWithExecutor(
        exec, String::fromBytes(endpoint).expect(), v1::s3v4::AccessKeyId{"AKIA_STATIC"_t},
        v1::s3v4::SecretAccessKey{"SECRET"_t}, "us-east-1"_t,
        "arn:aws:iam::123456789012:role/TestRole"_t, "session-name"_t, R"({"allow":true})"_t, 900s,
        1500ms
    );
    ASSERT_TRUE(parsed);
    const auto &creds = *parsed;

    EXPECT_EQ(capturedUrl, endpoint);
    const std::string authKey = "authorization";
    const std::string amzDateKey = "x-amz-date";
    const std::string contentTypeKey = "content-type";
    const std::string hostKey = "host";
    EXPECT_TRUE(capturedHeaders.contains(authKey));
    EXPECT_TRUE(capturedHeaders.contains(amzDateKey));
    EXPECT_EQ(capturedHeaders.at(contentTypeKey), "application/x-www-form-urlencoded");
    EXPECT_EQ(capturedHeaders.at(hostKey), std::string("sts.example.com"));
    EXPECT_EQ(capturedTimeout, 1500ms);

    EXPECT_TRUE(capturedBody.contains("Action=AssumeRole"));
    EXPECT_TRUE(capturedBody.contains("Version=2011-06-15"));
    EXPECT_TRUE(
        capturedBody.contains("RoleArn=arn%3Aaws%3Aiam%3A%3A123456789012%3Arole%2FTestRole")
    );
    EXPECT_TRUE(capturedBody.contains("RoleSessionName=session-name"));
    EXPECT_TRUE(capturedBody.contains("DurationSeconds=900"));
    EXPECT_TRUE(capturedBody.contains("Policy=%7B%22allow%22%3Atrue%7D"));

    EXPECT_EQ(creds.accessKeyId.GetUnderlying(), "AKIA_TEST_KEY"_t);
}

UTEST(S3StsClient, InvalidEndpointReturnsError)
{
    v1::detail::StsExecutor exec =
        [](const String &, const String &, const httpc::Headers &,
           std::chrono::milliseconds) -> v1::Expected<std::string, v1::StsError> {
        ADD_FAILURE() << "executor should not be called for invalid endpoint";
        return std::string{};
    };

    const auto parsed = v1::detail::fetchStsWithExecutor(
        exec, "https://["_t, v1::s3v4::AccessKeyId{"AKIA_STATIC"_t},
        v1::s3v4::SecretAccessKey{"SECRET"_t}, "us-east-1"_t,
        "arn:aws:iam::123456789012:role/TestRole"_t, "session-name"_t, R"({"allow":true})"_t, 900s,
        1500ms
    );
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error(), v1::StsError::kInvalidEndpoint);
}
