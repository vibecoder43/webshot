#include <map>
#include <string>
#include <vector>

#include <userver/utest/http_client.hpp>
#include <userver/utest/utest.hpp>
#include <userver/utils/datetime.hpp>

#include "s3/client.hpp"
#include "s3/sigv4_signer.hpp"
#include "text.hpp"

namespace ws {
namespace us = userver;
namespace datetime = us::utils::datetime;
} // namespace ws

using namespace ws;

using namespace std::chrono_literals;
using ws::s3::AccessKeyId;
using ws::s3::BuildCanonicalRequest;
using ws::s3::CanonicalRequestParts;
using ws::s3::Client;
using ws::s3::Config;
using ws::s3::Credentials;
using ws::s3::EncodeSlash;
using ws::s3::PercentEncode;
using ws::s3::SecretAccessKey;
using ws::s3::SignHeaders;
using ws::s3::SigParams;
using namespace text::literals;

UTEST(Sig, PercentEncodeBasicCharacters)
{
    EXPECT_EQ(PercentEncode("abcXYZ-_.~"_t, EncodeSlash::kNo), "abcXYZ-_.~"_t);
    EXPECT_EQ(PercentEncode(" "_t, EncodeSlash::kYes), "%20"_t);
    EXPECT_EQ(PercentEncode("!"_t, EncodeSlash::kYes), "%21"_t);
    EXPECT_EQ(PercentEncode("/"_t, EncodeSlash::kYes), "%2F"_t);
    EXPECT_EQ(PercentEncode("/"_t, EncodeSlash::kNo), "/"_t);
}

UTEST(Sig, BuildCanonicalRequestEncodesAndSortsQuery)
{
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("Param", "value value");
    query.emplace_back("Param", "value!");
    query.emplace_back("Another", "2");

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("host", "examplebucket.s3.amazonaws.com");

    CanonicalRequestParts parts = BuildCanonicalRequest(
        "GET", "/", query, headers, "UNSIGNED-PAYLOAD"
    );

    std::istringstream iss(parts.canonical_request);
    std::string method_line;
    std::string uri_line;
    std::string query_line;
    ASSERT_TRUE(static_cast<bool>(std::getline(iss, method_line)));
    ASSERT_TRUE(static_cast<bool>(std::getline(iss, uri_line)));
    ASSERT_TRUE(static_cast<bool>(std::getline(iss, query_line)));

    EXPECT_EQ(method_line, std::string{"GET"});
    EXPECT_EQ(uri_line, std::string{"/"});
    EXPECT_EQ(query_line, std::string{"Another=2&Param=value%20value&Param=value%21"});
}

UTEST(Sig, SignHeadersMatchesAwsExample)
{
    SigParams params;
    params.region = "us-east-1";
    params.service = "s3";
    params.access_key_id = AccessKeyId{"AKIAIOSFODNN7EXAMPLE"_t};
    params.secret_access_key = SecretAccessKey{"wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"_t};
    params.amz_date = "20130524T000000Z";
    params.date = "20130524";

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("host", "examplebucket.s3.amazonaws.com");
    headers.emplace_back("range", "bytes=0-9");

    const std::string payload_hash =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::vector<std::pair<String, String>> headers_text;
    headers_text.emplace_back("host"_t, "examplebucket.s3.amazonaws.com"_t);
    headers_text.emplace_back("range"_t, "bytes=0-9"_t);

    const auto signed_headers = SignHeaders(
        params, "GET"_t, "/test.txt"_t, /*query*/ {}, headers_text, *String::FromBytes(payload_hash)
    );

    auto it_date = signed_headers.find("x-amz-date");
    ASSERT_NE(it_date, signed_headers.end());
    const auto &[date_name, date_value] = *it_date;
    EXPECT_EQ(date_name, "x-amz-date");
    EXPECT_EQ(date_value, params.amz_date);

    auto it_payload = signed_headers.find("x-amz-content-sha256");
    ASSERT_NE(it_payload, signed_headers.end());
    const auto &[payload_name, payload_value] = *it_payload;
    EXPECT_EQ(payload_name, "x-amz-content-sha256");
    EXPECT_EQ(payload_value, payload_hash);

    auto it_auth = signed_headers.find("authorization");
    ASSERT_NE(it_auth, signed_headers.end());
    const auto &[auth_name, auth_value] = *it_auth;
    EXPECT_EQ(auth_name, "authorization");

    const std::string expected_auth =
        "AWS4-HMAC-SHA256 "
        "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
        "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
        "Signature=67fe34c8530db585abddc51067328adfedb6e42487d2566dc7d927d6e2722900";
    EXPECT_EQ(auth_value, expected_auth);
}

namespace {

std::map<std::string, std::string> ParseQuery(const std::string &url)
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

Config MakeConfig()
{
    Config cfg;
    cfg.endpoint = "https://examplebucket.s3.amazonaws.com"_t;
    cfg.region = "us-east-1"_t;
    cfg.timeout = 1000ms;
    cfg.virtual_hosted = false;
    return cfg;
}

Credentials MakeCreds()
{
    Credentials creds;
    creds.access_key_id = AccessKeyId{"AKIAIOSFODNN7EXAMPLE"_t};
    creds.secret_access_key = SecretAccessKey{"wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"_t};
    return creds;
}

} // namespace

UTEST(SigClient, PresignPathStyleClampsShortTtl)
{
    auto http_client = us::utest::CreateHttpClient();
    auto cfg = MakeConfig();
    auto creds = MakeCreds();
    auto client = std::make_shared<Client>(*http_client, cfg, creds, "examplebucket"_t);

    const auto expired = datetime::Now() - 1h;
    const std::string url = client->GenerateDownloadUrl(
        "test.txt", std::chrono::system_clock::to_time_t(expired), true
    );

    const auto params = ParseQuery(url);
    auto it = params.find("X-Amz-Expires");
    ASSERT_NE(it, params.end());
    const auto &[expires_name, expires_value] = *it;
    EXPECT_EQ(expires_name, "X-Amz-Expires");
    EXPECT_EQ(expires_value, std::string{"1"});
}

UTEST(SigClient, PresignPathStyleClampsLongTtl)
{
    auto http_client = us::utest::CreateHttpClient();
    auto cfg = MakeConfig();
    auto creds = MakeCreds();
    auto client = std::make_shared<Client>(*http_client, cfg, creds, "examplebucket"_t);

    const auto far_future = datetime::Now() + 14 * 24h;
    const std::string url = client->GenerateDownloadUrl(
        "test.txt", std::chrono::system_clock::to_time_t(far_future), true
    );

    const auto params = ParseQuery(url);
    auto it = params.find("X-Amz-Expires");
    ASSERT_NE(it, params.end());
    const auto &[expires_name, expires_value] = *it;
    EXPECT_EQ(expires_name, "X-Amz-Expires");
    EXPECT_EQ(expires_value, std::string{"604800"});
}

UTEST(SigClient, PresignPathStyleEncodesObjectKey)
{
    auto http_client = us::utest::CreateHttpClient();
    auto cfg = MakeConfig();
    auto creds = MakeCreds();
    auto client = std::make_shared<Client>(*http_client, cfg, creds, "examplebucket"_t);

    const auto soon = datetime::Now() + 120s;
    const std::string url = client->GenerateDownloadUrl(
        "folder/file with space.txt", std::chrono::system_clock::to_time_t(soon), true
    );

    const auto scheme_pos = url.find("://");
    ASSERT_NE(scheme_pos, std::string::npos);
    const auto path_start = url.find('/', scheme_pos + 3);
    ASSERT_NE(path_start, std::string::npos);
    const auto query_pos = url.find('?', path_start);
    const std::string path = url.substr(
        path_start, query_pos == std::string::npos ? std::string::npos : query_pos - path_start
    );

    EXPECT_EQ(path, std::string{"/examplebucket/folder/file%20with%20space.txt"});
}

UTEST(SigClient, ValidateVirtualHostBucketNameRequiresBucket)
{
    const auto result = ws::s3::detail::ValidateVirtualHostBucketName(String{});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.Error(), ws::s3::detail::VirtualHostPresignError::kMissingBucket);
}

UTEST(SigClient, VirtualHostUsesBucketInHost)
{
    auto http_client = us::utest::CreateHttpClient();
    auto cfg = MakeConfig();
    cfg.endpoint = "s3.example.com"_t;
    auto creds = MakeCreds();
    auto client = std::make_shared<Client>(*http_client, cfg, creds, "bucket-name"_t);

    const auto expires_at = datetime::Now() + 60s;
    const std::string url = client->GenerateDownloadUrlVirtualHostAddressing(
        "path/object", expires_at, "https"
    );

    const auto scheme_pos = url.find("://");
    ASSERT_NE(scheme_pos, std::string::npos);
    const auto host_start = scheme_pos + 3;
    const auto path_start = url.find('/', host_start);
    ASSERT_NE(path_start, std::string::npos);
    const std::string host = url.substr(host_start, path_start - host_start);

    EXPECT_EQ(host, std::string{"bucket-name.s3.example.com"});
}

UTEST(SigClient, UnsupportedOperationsThrow)
{
    auto http_client = us::utest::CreateHttpClient();
    auto cfg = MakeConfig();
    auto creds = MakeCreds();
    auto client = std::make_shared<Client>(*http_client, cfg, creds, "bucket-name"_t);

    us::s3api::ConnectionCfg new_cfg{50ms};
    EXPECT_NO_THROW(client->UpdateConfig(std::move(new_cfg)));
    EXPECT_EQ(client->GetBucketName(), std::string_view{"bucket-name"});
}

UTEST(SigClient, UploadPresignIncludesContentType)
{
    auto http_client = us::utest::CreateHttpClient();
    auto cfg = MakeConfig();
    cfg.endpoint = "s3.internal"_t;
    auto creds = MakeCreds();
    auto client = std::make_shared<Client>(*http_client, cfg, creds, "bucket-name"_t);

    const auto expires_at = datetime::Now() + 120s;
    const std::string url = client->GenerateUploadUrlVirtualHostAddressing(
        "ignored-body", "text/plain", "path/file.txt", expires_at, "http"
    );

    const auto params = ParseQuery(url);
    auto sh_it = params.find("X-Amz-SignedHeaders");
    ASSERT_NE(sh_it, params.end());
    const auto &[signed_headers_name, signed_headers_value] = *sh_it;
    EXPECT_EQ(signed_headers_name, "X-Amz-SignedHeaders");
    EXPECT_TRUE(signed_headers_value.contains("content-type"));
    EXPECT_TRUE(signed_headers_value.contains("host"));
}
