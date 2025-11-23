#include "s3/s3_sts_client.hpp"

#include "link.hpp"
#include "s3/sigv4_signer.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <ada/unicode.h>
#include <userver/clients/http/client.hpp>
#include <userver/clients/http/response.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/datetime.hpp>
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
    UINVARIANT(stsLink.url.type == ada::scheme::type::HTTPS, "STS endpoint must use https scheme");

    const std::string host = std::string(stsLink.url.get_host());

    std::string path = std::string(stsLink.url.get_pathname());
    if (path.empty())
        path = "/";

    std::vector<std::pair<std::string, std::string>> query;
    if (stsLink.url.has_search()) {
        std::string search = std::string(stsLink.url.get_search());
        if (!search.empty() && search.front() == '?')
            search.erase(search.begin());
        std::size_t pos = 0;
        while (pos < search.size()) {
            const auto amp = search.find('&', pos);
            const auto eq = search.find('=', pos);
            if (eq == std::string::npos)
                break;
            const std::string keyPart = search.substr(pos, eq - pos);
            const std::string valPart = search.substr(
                eq + 1, amp == std::string::npos ? std::string::npos : amp - eq - 1
            );
            const auto keyPercent = keyPart.find('%');
            const auto valPercent = valPart.find('%');
            const std::string key = ada::unicode::percent_decode(
                keyPart, keyPercent == std::string::npos ? std::string::npos : keyPercent
            );
            const std::string value = ada::unicode::percent_decode(
                valPart, valPercent == std::string::npos ? std::string::npos : valPercent
            );
            query.emplace_back(key, value);
            if (amp == std::string::npos)
                break;
            pos = amp + 1;
        }
    }

    const std::string url = std::string(stsLink.url.get_href());

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

    const auto now = userver::utils::datetime::Now();
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

    const auto signedHeaders = s3v4::SignHeaders(
        params, "POST", path, query, headersToSign, payloadHash
    );

    http::Headers headers;
    headers[userver::http::headers::kHost] = host;
    headers[userver::http::headers::kContentType] = "application/x-www-form-urlencoded";
    for (const auto &kv : signedHeaders)
        headers[kv.first] = kv.second;

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
