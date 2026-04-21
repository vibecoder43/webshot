#include "s3/s3_sts_client.hpp"

#include "integers.hpp"
#include "s3/s3_url_utils.hpp"
#include "s3/sigv4_signer.hpp"
#include "s3_credentials_types.hpp"
#include "text.hpp"
#include "try.hpp"
#include "url.hpp"
#include "userver_namespaces.hpp"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cctz/time_zone.h>

#include <userver/clients/http/client.hpp>
#include <userver/clients/http/response.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/datetime.hpp>
using namespace text::literals;

namespace v1 {

namespace {

[[nodiscard]] Expected<String, StsError> extractXmlTag(const String &xml, String tag)
{
    std::string open = "<";
    std::string xmlBytes(xml.view()), tagBytes(tag.view());
    open.append(tagBytes.data(), tagBytes.size()).push_back('>');
    std::string close = "</";
    close.append(tagBytes.data(), tagBytes.size()).push_back('>');
    const auto startPos = xmlBytes.find(open);
    ENSURE(startPos != std::string::npos, StsError::kXmlMissingTag);
    const auto valuePos = startPos + open.size();
    const auto endPos = xmlBytes.find(close, valuePos);
    ENSURE(endPos != std::string::npos, StsError::kXmlMissingClosingTag);
    return TRY_ERR_AS(
        String::fromBytes(xmlBytes.substr(valuePos, endPos - valuePos)), StsError::kInvalidUtf8
    );
}

[[nodiscard]] Expected<std::chrono::system_clock::time_point, StsError>
parseExpiration(const String &expiration)
{
    std::chrono::system_clock::time_point expiresAt;
    if (!cctz::parse(
            datetime::kRfc3339Format, std::to_string(expiration), cctz::utc_time_zone(), &expiresAt
        )) {
        return Unex(StsError::kInvalidExpiration);
    }
    return expiresAt;
}

} // namespace

Expected<StsCredentials, StsError> StsCredentials::fromXml(const String &xml)
{
    auto accessKeyId = TRY(extractXmlTag(xml, "AccessKeyId"_t));
    auto secretAccessKey = TRY(extractXmlTag(xml, "SecretAccessKey"_t));
    auto sessionToken = TRY(extractXmlTag(xml, "SessionToken"_t));
    auto expiration = TRY(extractXmlTag(xml, "Expiration"_t));
    auto expiresAt = TRY(parseExpiration(expiration));

    StsCredentials creds{
        s3v4::AccessKeyId(std::move(accessKeyId)),
        s3v4::SecretAccessKey(std::move(secretAccessKey)),
        s3v4::SessionToken(std::move(sessionToken)),
        expiresAt,
    };
    return creds;
}

Expected<StsCredentials, StsError> detail::fetchStsWithExecutor(
    const StsExecutor &exec, const String &stsEndpoint, const s3v4::AccessKeyId &staticAccessKeyId,
    const s3v4::SecretAccessKey &staticSecretAccessKey, const String &region, const String &roleArn,
    const String &roleSessionName, const String &policyJson, std::chrono::seconds duration,
    std::chrono::milliseconds timeout
)
{
    const auto stsUrl = TRY_OK_OR(
        s3v4::parseUrlWithDefaultHttpScheme(stsEndpoint), StsError::kInvalidEndpoint
    );
    invariant(stsUrl.isHttps(), "STS endpoint must use https scheme");

    const auto host = stsUrl.host();

    auto path = stsUrl.pathname();
    if (path.empty())
        path = "/"_t;

    std::vector<std::pair<String, String>> query;
    if (stsUrl.hasSearch()) {
        const auto search = stsUrl.search();
        query = TRY_ERR_AS(s3v4::decodeQueryString(search), StsError::kInvalidQuery);
    }
    String body;
    auto appendParam = [&body](const String &name, const String &value) {
        if (!body.empty())
            body += "&"_t;
        body += name;
        body += "="_t;
        body += s3v4::percentEncode(value, s3v4::EncodeSlash::kYes);
    };
    appendParam("Action"_t, "AssumeRole"_t);
    appendParam("Version"_t, "2011-06-15"_t);
    appendParam("RoleArn"_t, roleArn);
    appendParam("RoleSessionName"_t, roleSessionName);
    appendParam("DurationSeconds"_t, text::format("{}", duration.count()));
    appendParam("Policy"_t, policyJson);

    const String payloadHash = s3v4::sha256Hex(body.view());

    const auto now = datetime::Now();
    s3v4::SigV4Params params(
        std::to_string(region), "sts", staticAccessKeyId, staticSecretAccessKey, {}, now
    );

    std::vector<std::pair<String, String>> headersToSign;
    headersToSign.emplace_back("host"_t, host);
    const auto kUrlEncoded = "application/x-www-form-urlencoded"_t;
    headersToSign.emplace_back("content-type"_t, kUrlEncoded);
    const auto signedHeaders = s3v4::signHeaders(
        params, "POST"_t, path, query, headersToSign, payloadHash
    );
    httpc::Headers headers;
    headers[us::http::headers::kHost] = std::to_string(host);
    headers[us::http::headers::kContentType] = std::to_string(kUrlEncoded);
    for (const auto &kv : signedHeaders)
        headers[kv.first] = kv.second;
    const auto response = TRY(exec(stsUrl.href(), body, headers, timeout));

    return StsCredentials::fromXml(TRY_ERR_AS(String::fromBytes(response), StsError::kInvalidUtf8));
}

Expected<StsCredentials, StsError> fetchStsCredentials(
    httpc::Client &httpClient, const String &stsEndpoint,
    const s3v4::AccessKeyId &staticAccessKeyId, const s3v4::SecretAccessKey &staticSecretAccessKey,
    const String &region, const String &roleArn, const String &roleSessionName,
    const String &policyJson, std::chrono::seconds duration, std::chrono::milliseconds timeout
)
{
    detail::StsExecutor exec = [&httpClient](
                                   const String &url, const String &body,
                                   const httpc::Headers &headers,
                                   std::chrono::milliseconds timeoutMs
                               ) -> Expected<std::string, StsError> {
        auto urlBytes = std::to_string(url);
        auto bodyBytes = std::to_string(body);
        auto resp = httpClient.CreateNotSignedRequest()
                        .post(urlBytes, std::move(bodyBytes))
                        .headers(headers)
                        .timeout(timeoutMs)
                        .perform();
        const auto status = numericCast<int>(resp->status_code());
        if (status >= 300) {
            LOG_ERROR() << std::format("STS request failed: url={}, status={}", urlBytes, status);
            return Unex(StsError::kHttpFailure);
        }
        return resp->body();
    };

    return detail::fetchStsWithExecutor(
        exec, stsEndpoint, staticAccessKeyId, staticSecretAccessKey, region, roleArn,
        roleSessionName, policyJson, duration, timeout
    );
}

} // namespace v1
