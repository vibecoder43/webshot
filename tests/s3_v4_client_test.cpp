#include <string>
#include <vector>

#include <userver/utest/utest.hpp>

#include "s3/sigv4_signer.hpp"

using v1::s3v4::BuildCanonicalRequest;
using v1::s3v4::CanonicalRequestParts;
using v1::s3v4::PercentEncode;
using v1::s3v4::SignHeaders;
using v1::s3v4::SigV4Params;

UTEST(S3SigV4, PercentEncodeBasicCharacters)
{
    EXPECT_EQ(PercentEncode("abcXYZ-_.~", false), std::string{"abcXYZ-_.~"});
    EXPECT_EQ(PercentEncode(" ", true), std::string{"%20"});
    EXPECT_EQ(PercentEncode("!", true), std::string{"%21"});
    EXPECT_EQ(PercentEncode("/", true), std::string{"%2F"});
    EXPECT_EQ(PercentEncode("/", false), std::string{"/"});
}

UTEST(S3SigV4, BuildCanonicalRequestEncodesAndSortsQuery)
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
    params.accessKeyId = "AKIAIOSFODNN7EXAMPLE";
    params.secretAccessKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    params.amzDate = "20130524T000000Z";
    params.date = "20130524";

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("host", "examplebucket.s3.amazonaws.com");
    headers.emplace_back("range", "bytes=0-9");

    const std::string payloadHash =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    const auto signedHeaders = SignHeaders(
        params, "GET", "/test.txt", /*query*/ {}, headers, payloadHash
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
