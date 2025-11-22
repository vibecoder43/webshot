#include "s3/s3_sts_client.hpp"

#include "link.hpp"
#include "s3/sigv4_signer.hpp"

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
#include <userver/utils/datetime/from_string_saturating.hpp>

namespace http = userver::clients::http;

namespace v1 {

namespace {

[[nodiscard]] std::string ExtractXmlTag(const std::string &xml, std::string_view tag)
{
    std::string open = "<";
    open.append(tag.data(), tag.size()).push_back('>');
    std::string close = "</";
    close.append(tag.data(), tag.size()).push_back('>');
    const auto startPos = xml.find(open);
    if (startPos == std::string::npos)
        throw std::runtime_error("STS XML missing tag");
    const auto valuePos = startPos + open.size();
    const auto endPos = xml.find(close, valuePos);
    if (endPos == std::string::npos)
        throw std::runtime_error("STS XML missing closing tag");
    return xml.substr(valuePos, endPos - valuePos);
}

} // namespace

StsCredentials::StsCredentials(const std::string &xml)
    : accessKeyId(s3v4::AccessKeyId{ExtractXmlTag(xml, "AccessKeyId")}),
      secretAccessKey(s3v4::SecretAccessKey{ExtractXmlTag(xml, "SecretAccessKey")}),
      sessionToken(s3v4::SessionToken{ExtractXmlTag(xml, "SessionToken")}),
      expiresAt(
          userver::utils::datetime::FromRfc3339StringSaturating(ExtractXmlTag(xml, "Expiration"))
      )
{
}

StsCredentials FetchStsCredentials(
    http::Client &httpClient, const std::string &stsEndpoint,
    const s3v4::AccessKeyId &staticAccessKeyId, const s3v4::SecretAccessKey &staticSecretAccessKey,
    const std::string &region, const std::string &roleArn, const std::string &roleSessionName,
    const std::string &policyJson, std::chrono::seconds duration, std::chrono::milliseconds timeout
)
{
    const auto stsLink = Link::fromUserInput(stsEndpoint, static_cast<size_t>(stsEndpoint.size()));

    std::string schemeRaw = std::string(stsLink.url.get_protocol());
    std::string scheme;
    if (schemeRaw.empty()) {
        scheme = "https";
    } else {
        if (!schemeRaw.empty() && schemeRaw.back() == ':')
            schemeRaw.pop_back();
        if (schemeRaw == "https") {
            scheme = "https";
        } else {
            throw std::runtime_error("STS endpoint must use https scheme");
        }
    }
    std::string host = stsLink.host();
    std::string path = std::string(stsLink.url.get_pathname());
    if (path.empty())
        path = "/";

    std::string body;
    body.reserve(512);
    auto appendParam = [&body](std::string_view name, std::string_view value) {
        if (!body.empty())
            body.push_back('&');
        body.append(name);
        body.push_back('=');
        body.append(s3v4::PercentEncode(value, true));
    };
    appendParam("Action", "AssumeRole");
    appendParam("Version", "2011-06-15");
    appendParam("RoleArn", roleArn);
    appendParam("RoleSessionName", roleSessionName);
    appendParam("DurationSeconds", std::to_string(duration.count()));
    appendParam("Policy", policyJson);

    const std::string payloadHash = s3v4::Sha256Hex(body);

    const auto now = std::chrono::system_clock::now();
    s3v4::SigV4Params params;
    params.region = region;
    params.service = "sts";
    params.accessKeyId = staticAccessKeyId;
    params.secretAccessKey = staticSecretAccessKey;
    params.amzDate = s3v4::ToAmzDateUtc(now);
    params.date = s3v4::ToDateStampUtc(now);

    std::vector<std::pair<std::string, std::string>> headersToSign;
    headersToSign.emplace_back("host", host);
    headersToSign.emplace_back("content-type", "application/x-www-form-urlencoded");

    const std::vector<std::pair<std::string, std::string>> query;
    const auto signedHeaders = s3v4::SignHeaders(
        params, "POST", path, query, headersToSign, payloadHash
    );

    http::Headers headers;
    headers[userver::http::headers::kHost] = host;
    headers[userver::http::headers::kContentType] = "application/x-www-form-urlencoded";
    for (const auto &kv : signedHeaders)
        headers[kv.first] = kv.second;

    const std::string url = fmt::format("{}://{}{}", scheme, host, path);

    auto resp = httpClient.CreateNotSignedRequest()
                    .post(url, body)
                    .headers(headers)
                    .timeout(timeout)
                    .perform();
    resp->raise_for_status();
    const std::string xml = resp->body();

    return StsCredentials{xml};
}

} // namespace v1
