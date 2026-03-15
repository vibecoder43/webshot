#include <map>
#include <string>
#include <vector>

#include <userver/utest/http_client.hpp>
#include <userver/utest/utest.hpp>
#include <userver/utils/datetime.hpp>

#include "s3/s3_v4_client.hpp"
#include "s3/sigv4_signer.hpp"
#include "text.hpp"

using v1::s3v4::AccessKeyId;
using v1::s3v4::buildCanonicalRequest;
using v1::s3v4::CanonicalRequestParts;
using v1::s3v4::percentEncode;
using v1::s3v4::S3Credentials;
using v1::s3v4::S3V4Client;
using v1::s3v4::S3V4Config;
using v1::s3v4::SecretAccessKey;
using v1::s3v4::signHeaders;
using v1::s3v4::SigV4Params;
using namespace text::literals;

UTEST(S3SigV4, PercentEncodeBasicCharacters)
{
    EXPECT_EQ(percentEncode("abcXYZ-_.~"_t, false).view(), std::string{"abcXYZ-_.~"});
    EXPECT_EQ(percentEncode(" "_t, true).view(), std::string{"%20"});
    EXPECT_EQ(percentEncode("!"_t, true).view(), std::string{"%21"});
    EXPECT_EQ(percentEncode("/"_t, true).view(), std::string{"%2F"});
    EXPECT_EQ(percentEncode("/"_t, false).view(), std::string{"/"});
}

UTEST(S3SigV4, BuildCanonicalRequestEncodesAndSortsQuery)
{
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("Param", "value value");
    query.emplace_back("Param", "value!");
    query.emplace_back("Another", "2");

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("host", "examplebucket.s3.amazonaws.com");

    CanonicalRequestParts parts = buildCanonicalRequest(
        "GET", "/", query, headers, "UNSIGNED-PAYLOAD"
    );

    std::istringstream iss(parts.canonicalRequest);
    std::string methodLine;
    std::string uriLine;
    std::string queryLine;
    ASSERT_TRUE(static_cast<bool>(std::getline(iss, methodLine)));
    ASSERT_TRUE(static_cast<bool>(std::getline(iss, uriLine)));
    ASSERT_TRUE(static_cast<bool>(std::getline(iss, queryLine)));

    EXPECT_EQ(methodLine, std::string{"GET"});
    EXPECT_EQ(uriLine, std::string{"/"});
    EXPECT_EQ(queryLine, std::string{"Another=2&Param=value%20value&Param=value%21"});
}

UTEST(S3SigV4, SignHeadersMatchesAwsExample)
{
    SigV4Params params;
    params.region = "us-east-1";
    params.service = "s3";
    params.accessKeyId = AccessKeyId{"AKIAIOSFODNN7EXAMPLE"_t};
    params.secretAccessKey = SecretAccessKey{"wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"_t};
    params.amzDate = "20130524T000000Z";
    params.date = "20130524";

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("host", "examplebucket.s3.amazonaws.com");
    headers.emplace_back("range", "bytes=0-9");

    const std::string payloadHash =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::vector<std::pair<String, String>> headersText;
    headersText.emplace_back("host"_t, "examplebucket.s3.amazonaws.com"_t);
    headersText.emplace_back("range"_t, "bytes=0-9"_t);

    const auto signedHeaders = signHeaders(
        params, "GET"_t, "/test.txt"_t, /*query*/ {}, headersText,
        String::fromBytesThrow(payloadHash)
    );

    auto itDate = signedHeaders.find("x-amz-date");
    ASSERT_NE(itDate, signedHeaders.end());
    EXPECT_EQ(itDate->second, params.amzDate);

    auto itPayload = signedHeaders.find("x-amz-content-sha256");
    ASSERT_NE(itPayload, signedHeaders.end());
    EXPECT_EQ(itPayload->second, payloadHash);

    auto itAuth = signedHeaders.find("authorization");
    ASSERT_NE(itAuth, signedHeaders.end());

    const std::string expectedAuth =
        "AWS4-HMAC-SHA256 "
        "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
        "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
        "Signature=67fe34c8530db585abddc51067328adfedb6e42487d2566dc7d927d6e2722900";
    EXPECT_EQ(itAuth->second, expectedAuth);
}

namespace {

std::map<std::string, std::string> parseQuery(const std::string &url)
{
    std::map<std::string, std::string> out;
    const auto pos = url.find('?');
    if (pos == std::string::npos)
        return out;
    const std::string query = url.substr(pos + 1);
    std::size_t start = 0;
    while (start < query.size()) {
        const auto amp = query.find('&', start);
        const auto eq = query.find('=', start);
        if (eq == std::string::npos)
            break;
        const std::string key = query.substr(start, eq - start);
        const std::string value = query.substr(
            eq + 1, amp == std::string::npos ? std::string::npos : amp - eq - 1
        );
        out[key] = value;
        if (amp == std::string::npos)
            break;
        start = amp + 1;
    }
    return out;
}

S3V4Config makeConfig()
{
    S3V4Config cfg;
    cfg.endpoint = "https://examplebucket.s3.amazonaws.com"_t;
    cfg.region = "us-east-1"_t;
    cfg.timeout = std::chrono::milliseconds(1000);
    cfg.virtualHosted = false;
    return cfg;
}

S3Credentials makeCreds()
{
    S3Credentials creds;
    creds.accessKeyId = AccessKeyId{"AKIAIOSFODNN7EXAMPLE"_t};
    creds.secretAccessKey = SecretAccessKey{"wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"_t};
    return creds;
}

} // namespace

UTEST(S3SigV4Client, PresignPathStyleClampsShortTtl)
{
    auto httpClient = userver::utest::CreateHttpClient();
    auto cfg = makeConfig();
    auto creds = makeCreds();
    auto client = std::make_shared<S3V4Client>(*httpClient, cfg, creds, "examplebucket"_t);

    const std::time_t expired = std::time(nullptr) - 3600;
    const std::string url = client->GenerateDownloadUrl("test.txt", expired, true);

    const auto params = parseQuery(url);
    auto it = params.find("X-Amz-Expires");
    ASSERT_NE(it, params.end());
    EXPECT_EQ(it->second, std::string{"1"});
}

UTEST(S3SigV4Client, PresignPathStyleClampsLongTtl)
{
    auto httpClient = userver::utest::CreateHttpClient();
    auto cfg = makeConfig();
    auto creds = makeCreds();
    auto client = std::make_shared<S3V4Client>(*httpClient, cfg, creds, "examplebucket"_t);

    constexpr std::time_t kTwoWeeksSeconds = std::time_t{14} * std::time_t{24} * std::time_t{60} *
                                             std::time_t{60};
    const std::time_t farFuture = std::time(nullptr) + kTwoWeeksSeconds;
    const std::string url = client->GenerateDownloadUrl("test.txt", farFuture, true);

    const auto params = parseQuery(url);
    auto it = params.find("X-Amz-Expires");
    ASSERT_NE(it, params.end());
    EXPECT_EQ(it->second, std::string{"604800"});
}

UTEST(S3SigV4Client, PresignPathStyleEncodesObjectKey)
{
    auto httpClient = userver::utest::CreateHttpClient();
    auto cfg = makeConfig();
    auto creds = makeCreds();
    auto client = std::make_shared<S3V4Client>(*httpClient, cfg, creds, "examplebucket"_t);

    const std::time_t soon = std::time(nullptr) + 120;
    const std::string url = client->GenerateDownloadUrl("folder/file with space.txt", soon, true);

    const auto schemePos = url.find("://");
    ASSERT_NE(schemePos, std::string::npos);
    const auto pathStart = url.find('/', schemePos + 3);
    ASSERT_NE(pathStart, std::string::npos);
    const auto queryPos = url.find('?', pathStart);
    const std::string path = url.substr(
        pathStart, queryPos == std::string::npos ? std::string::npos : queryPos - pathStart
    );

    EXPECT_EQ(path, std::string{"/examplebucket/folder/file%20with%20space.txt"});
}

UTEST(S3SigV4Client, VirtualHostRequiresBucket)
{
    auto httpClient = userver::utest::CreateHttpClient();
    auto cfg = makeConfig();
    auto creds = makeCreds();
    auto client = std::make_shared<S3V4Client>(*httpClient, cfg, creds, String());

    const auto expiresAt = userver::utils::datetime::Now() + std::chrono::seconds(60);
    EXPECT_THROW(
        client->GenerateDownloadUrlVirtualHostAddressing("obj", expiresAt, "https"),
        std::runtime_error
    );
}

UTEST(S3SigV4Client, VirtualHostUsesBucketInHost)
{
    auto httpClient = userver::utest::CreateHttpClient();
    auto cfg = makeConfig();
    cfg.endpoint = "s3.example.com"_t;
    auto creds = makeCreds();
    auto client = std::make_shared<S3V4Client>(*httpClient, cfg, creds, "bucket-name"_t);

    const auto expiresAt = userver::utils::datetime::Now() + std::chrono::seconds(60);
    const std::string url = client->GenerateDownloadUrlVirtualHostAddressing(
        "path/object", expiresAt, "https"
    );

    const auto schemePos = url.find("://");
    ASSERT_NE(schemePos, std::string::npos);
    const auto hostStart = schemePos + 3;
    const auto pathStart = url.find('/', hostStart);
    ASSERT_NE(pathStart, std::string::npos);
    const std::string host = url.substr(hostStart, pathStart - hostStart);

    EXPECT_EQ(host, std::string{"bucket-name.s3.example.com"});
}

UTEST(S3SigV4Client, UnsupportedOperationsThrow)
{
    auto httpClient = userver::utest::CreateHttpClient();
    auto cfg = makeConfig();
    auto creds = makeCreds();
    auto client = std::make_shared<S3V4Client>(*httpClient, cfg, creds, "bucket-name"_t);

    using S3Client = userver::s3api::Client;
    S3Client::HeadersDataResponse headersResponse;
    S3Client::HeaderDataRequest headerRequest;

    EXPECT_THROW(client->GetObject("key", {}, &headersResponse, headerRequest), std::runtime_error);
    EXPECT_THROW(
        client->TryGetObject("key", {}, &headersResponse, headerRequest), std::runtime_error
    );
    EXPECT_THROW(
        client->GetPartialObject("key", "bytes=0-1", {}, &headersResponse, headerRequest),
        std::runtime_error
    );
    EXPECT_THROW(
        client->TryGetPartialObject("key", "bytes=0-1", {}, &headersResponse, headerRequest),
        std::runtime_error
    );
    EXPECT_THROW(client->CopyObject("src", "dst", {}), std::runtime_error);
    EXPECT_THROW(client->CopyObject("bucket", "src", "dst", {}), std::runtime_error);
    EXPECT_THROW(client->ListBucketContents("bucket", 1, "", ""), std::runtime_error);
    EXPECT_THROW(client->ListBucketContentsParsed("bucket"), std::runtime_error);
    EXPECT_THROW(client->ListBucketDirectories("bucket"), std::runtime_error);

    userver::s3api::ConnectionCfg newCfg{std::chrono::milliseconds{50}};
    EXPECT_NO_THROW(client->UpdateConfig(std::move(newCfg)));
    EXPECT_EQ(client->GetBucketName(), std::string_view{"bucket-name"});
}

UTEST(S3SigV4Client, UploadPresignIncludesContentType)
{
    auto httpClient = userver::utest::CreateHttpClient();
    auto cfg = makeConfig();
    cfg.endpoint = "s3.internal"_t;
    auto creds = makeCreds();
    auto client = std::make_shared<S3V4Client>(*httpClient, cfg, creds, "bucket-name"_t);

    const auto expiresAt = userver::utils::datetime::Now() + std::chrono::seconds(120);
    const std::string url = client->GenerateUploadUrlVirtualHostAddressing(
        "ignored-body", "text/plain", "path/file.txt", expiresAt, "http"
    );

    const auto params = parseQuery(url);
    auto shIt = params.find("X-Amz-SignedHeaders");
    ASSERT_NE(shIt, params.end());
    EXPECT_NE(shIt->second.find("content-type"), std::string::npos);
    EXPECT_NE(shIt->second.find("host"), std::string::npos);
}
