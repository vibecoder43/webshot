#include "s3/s3_sts_client.hpp"

#include "integers.hpp"
#include "link.hpp"
#include "s3/s3_url_utils.hpp"
#include "s3/sigv4_signer.hpp"
#include "s3_credentials_types.hpp"
#include "text.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <userver/clients/http/client.hpp>
#include <userver/clients/http/response.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/datetime/from_string_saturating.hpp>

namespace http = userver::clients::http;
using namespace text::literals;

namespace v1 {

namespace {

[[nodiscard]] String extractXmlTag(const String &xml, String tag)
{
    std::string open = "<";
    std::string xmlBytes(xml.view()), tagBytes(tag.view());
    open.append(tagBytes.data(), tagBytes.size()).push_back('>');
    std::string close = "</";
    close.append(tagBytes.data(), tagBytes.size()).push_back('>');
    const auto startPos = xmlBytes.find(open);
    if (startPos == std::string::npos)
        throw std::runtime_error("STS XML missing tag");
    const auto valuePos = startPos + open.size();
    const auto endPos = xmlBytes.find(close, valuePos);
    if (endPos == std::string::npos)
        throw std::runtime_error("STS XML missing closing tag");
    return String::fromBytesThrow(xmlBytes.substr(valuePos, endPos - valuePos));
}

} // namespace

StsCredentials::StsCredentials(const String &xml)
    : accessKeyId(s3v4::AccessKeyId(extractXmlTag(xml, "AccessKeyId"_t))),
      secretAccessKey(s3v4::SecretAccessKey(extractXmlTag(xml, "SecretAccessKey"_t))),
      sessionToken(s3v4::SessionToken(extractXmlTag(xml, "SessionToken"_t))),
      expiresAt(
          userver::utils::datetime::FromRfc3339StringSaturating(
              std::string(extractXmlTag(xml, "Expiration"_t).view())
          )
      )
{
}

StsCredentials detail::fetchStsWithExecutor(
    const StsExecutor &exec, const String &stsEndpoint, const s3v4::AccessKeyId &staticAccessKeyId,
    const s3v4::SecretAccessKey &staticSecretAccessKey, const String &region, const String &roleArn,
    const String &roleSessionName, const String &policyJson, std::chrono::seconds duration,
    std::chrono::milliseconds timeout
)
{
    const auto stsLink = Link::fromText(stsEndpoint, stsEndpoint.sizeBytes());
    UINVARIANT(stsLink.url.isHttps(), "STS endpoint must use https scheme");

    const auto host = stsLink.url.host();

    auto path = stsLink.url.pathname();
    if (path.empty())
        path = "/"_t;

    std::vector<std::pair<String, String>> query;
    if (stsLink.url.hasSearch()) {
        const auto search = stsLink.url.search();
        query = s3v4::decodeQueryString(search);
    }
    String body;
    auto appendParam = [&body](const String &name, const String &value) {
        if (!body.empty())
            body += "&"_t;
        body += name;
        body += "="_t;
        body += s3v4::percentEncode(value, true);
    };
    appendParam("Action"_t, "AssumeRole"_t);
    appendParam("Version"_t, "2011-06-15"_t);
    appendParam("RoleArn"_t, roleArn);
    appendParam("RoleSessionName"_t, roleSessionName);
    appendParam("DurationSeconds"_t, String::fromBytesThrow(std::to_string(duration.count())));
    appendParam("Policy"_t, policyJson);

    const String payloadHash = s3v4::sha256Hex(body.view());

    const auto now = userver::utils::datetime::Now();
    s3v4::SigV4Params params(
        std::string(region.view()), "sts", staticAccessKeyId, staticSecretAccessKey, {}, now
    );

    std::vector<std::pair<String, String>> headersToSign;
    headersToSign.emplace_back("host"_t, host);
    const auto kUrlEncoded = "application/x-www-form-urlencoded"_t;
    headersToSign.emplace_back("content-type"_t, kUrlEncoded);
    const auto signedHeaders = s3v4::signHeaders(
        params, "POST"_t, path, query, headersToSign, payloadHash
    );
    http::Headers headers;
    headers[userver::http::headers::kHost] = std::string(host.view());
    headers[userver::http::headers::kContentType] = std::string(kUrlEncoded.view());
    for (const auto &kv : signedHeaders)
        headers[kv.first] = kv.second;
    return StsCredentials{String::fromBytesThrow(exec(stsLink.httpsUrl(), body, headers, timeout))};
}

StsCredentials fetchStsCredentials(
    http::Client &httpClient, const String &stsEndpoint, const s3v4::AccessKeyId &staticAccessKeyId,
    const s3v4::SecretAccessKey &staticSecretAccessKey, const String &region, const String &roleArn,
    const String &roleSessionName, const String &policyJson, std::chrono::seconds duration,
    std::chrono::milliseconds timeout
)
{
    detail::StsExecutor exec = [&httpClient](
                                   const String &url, const String &body,
                                   const http::Headers &headers, std::chrono::milliseconds timeoutMs
                               ) {
        auto urlBytes = std::string(url.view());
        auto bodyBytes = std::string(body.view());
        auto resp = httpClient.CreateNotSignedRequest()
                        .post(urlBytes, std::move(bodyBytes))
                        .headers(headers)
                        .timeout(timeoutMs)
                        .perform();
        const auto status = numericCast<int>(resp->status_code());
        if (status >= 300) {
            const auto bodyOut = resp->body();
            if (const auto bodyUtf8 = String::fromBytes(bodyOut)) {
                LOG_ERROR() << fmt::format(
                    "STS request failed: url={}, status={}, body={}", urlBytes, status,
                    bodyUtf8.value()
                );
            } else {
                LOG_ERROR() << fmt::format(
                    "STS request failed: url={}, status={}, body is not valid UTF-8 ({} bytes)",
                    urlBytes, status, bodyOut.size()
                );
            }
        }
        resp->raise_for_status();
        return resp->body();
    };

    return detail::fetchStsWithExecutor(
        exec, stsEndpoint, staticAccessKeyId, staticSecretAccessKey, region, roleArn,
        roleSessionName, policyJson, duration, timeout
    );
}

} // namespace v1
