#include "s3/sts_client.hpp"

#include "integers.hpp"
#include "invariant.hpp"
#include "s3/credentials_types.hpp"
#include "s3/sigv4_signer.hpp"
#include "s3/url_utils.hpp"
#include "text.hpp"
#include "try.hpp"
#include "url.hpp"

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

namespace ws {

namespace us = userver;
namespace datetime = us::utils::datetime;
namespace httpc = us::clients::http;
using text::ToBytes;

namespace {

[[nodiscard]] Expected<String, StsError> ExtractXmlTag(const String &xml, String tag)
{
    std::string open = "<";
    std::string xml_bytes(xml.View()), tag_bytes(tag.View());
    open.append(tag_bytes.data(), tag_bytes.size()).push_back('>');
    std::string close = "</";
    close.append(tag_bytes.data(), tag_bytes.size()).push_back('>');
    const auto start_pos = xml_bytes.find(open);
    ENSURE(start_pos != std::string::npos, StsError::kXmlMissingTag);
    const auto value_pos = start_pos + open.size();
    const auto end_pos = xml_bytes.find(close, value_pos);
    ENSURE(end_pos != std::string::npos, StsError::kXmlMissingClosingTag);
    return TRY_ERR_AS(
        String::FromBytes(xml_bytes.substr(value_pos, end_pos - value_pos)), StsError::kInvalidUtf8
    );
}

[[nodiscard]] Expected<std::chrono::system_clock::time_point, StsError>
ParseExpiration(const String &expiration)
{
    std::chrono::system_clock::time_point expires_at;
    if (!cctz::parse(
            datetime::kRfc3339Format, ToBytes(expiration), cctz::utc_time_zone(), &expires_at
        )) {
        return Unex(StsError::kInvalidExpiration);
    }
    return expires_at;
}

} // namespace

Expected<StsCredentials, StsError> StsCredentials::FromXml(const String &xml)
{
    auto access_key_id = TRY(ExtractXmlTag(xml, "AccessKeyId"_t));
    auto secret_access_key = TRY(ExtractXmlTag(xml, "SecretAccessKey"_t));
    auto session_token = TRY(ExtractXmlTag(xml, "SessionToken"_t));
    auto expiration = TRY(ExtractXmlTag(xml, "Expiration"_t));
    auto expires_at = TRY(ParseExpiration(expiration));

    StsCredentials creds{
        s3::AccessKeyId(std::move(access_key_id)),
        s3::SecretAccessKey(std::move(secret_access_key)),
        s3::SessionToken(std::move(session_token)),
        expires_at,
    };
    return creds;
}

Expected<StsCredentials, StsError> detail::FetchStsWithExecutor(
    const StsExecutor &exec, const String &sts_endpoint,
    const s3::AccessKeyId &static_access_key_id,
    const s3::SecretAccessKey &static_secret_access_key, const String &region,
    const String &role_arn, const String &role_session_name, const String &policy_json,
    std::chrono::seconds duration, std::chrono::milliseconds timeout
)
{
    const auto sts_url = TRY_OK_OR(
        s3::ParseUrlWithDefaultHttpScheme(sts_endpoint), StsError::kInvalidEndpoint
    );
    Invariant(sts_url.IsHttps(), "STS endpoint must use https scheme"_t);

    const auto host = sts_url.Host();

    auto path = sts_url.Pathname();
    if (path.Empty())
        path = "/"_t;

    std::vector<std::pair<String, String>> query;
    if (sts_url.HasSearch()) {
        const auto search = sts_url.Search();
        query = TRY_ERR_AS(s3::DecodeQueryString(search), StsError::kInvalidQuery);
    }
    String body;
    auto append_param = [&body](const String &name, const String &value) {
        if (!body.Empty())
            body += "&"_t;
        body += name;
        body += "="_t;
        body += s3::PercentEncode(value, s3::EncodeSlash::kYes);
    };
    append_param("Action"_t, "AssumeRole"_t);
    append_param("Version"_t, "2011-06-15"_t);
    append_param("RoleArn"_t, role_arn);
    append_param("RoleSessionName"_t, role_session_name);
    append_param("DurationSeconds"_t, text::Format("{}", duration.count()));
    append_param("Policy"_t, policy_json);

    const String payload_hash = s3::Sha256Hex(body.View());

    const auto now = datetime::Now();
    s3::SigParams params(
        ToBytes(region), "sts", static_access_key_id, static_secret_access_key, {}, now
    );

    std::vector<std::pair<String, String>> headers_to_sign;
    headers_to_sign.emplace_back("host"_t, host);
    const auto url_encoded = "application/x-www-form-urlencoded"_t;
    headers_to_sign.emplace_back("content-type"_t, url_encoded);
    const auto signed_headers = s3::SignHeaders(
        params, "POST"_t, path, query, headers_to_sign, payload_hash
    );
    httpc::Headers headers;
    headers[us::http::headers::kHost] = ToBytes(host);
    headers[us::http::headers::kContentType] = ToBytes(url_encoded);
    for (const auto &[name, value] : signed_headers)
        headers[name] = value;
    const auto response = TRY(exec(sts_url.Href(), body, headers, timeout));

    return StsCredentials::FromXml(TRY_ERR_AS(String::FromBytes(response), StsError::kInvalidUtf8));
}

Expected<StsCredentials, StsError> FetchStsCredentials(
    httpc::Client &http_client, const String &sts_endpoint,
    const s3::AccessKeyId &static_access_key_id,
    const s3::SecretAccessKey &static_secret_access_key, const String &region,
    const String &role_arn, const String &role_session_name, const String &policy_json,
    std::chrono::seconds duration, std::chrono::milliseconds timeout
)
{
    detail::StsExecutor exec = [&http_client](
                                   const String &url, const String &body,
                                   const httpc::Headers &headers,
                                   std::chrono::milliseconds timeout_ms
                               ) -> Expected<std::string, StsError> {
        auto url_bytes = ToBytes(url);
        auto body_bytes = ToBytes(body);
        auto resp = http_client.CreateNotSignedRequest()
                        .post(url_bytes, std::move(body_bytes))
                        .headers(headers)
                        .timeout(timeout_ms)
                        .perform();
        const auto status = NumericCast<int>(resp->status_code());
        if (status >= 300) {
            LOG_ERROR() << std::format("STS request failed: url={}, status={}", url_bytes, status);
            return Unex(StsError::kHttpFailure);
        }
        return resp->body();
    };

    return detail::FetchStsWithExecutor(
        exec, sts_endpoint, static_access_key_id, static_secret_access_key, region, role_arn,
        role_session_name, policy_json, duration, timeout
    );
}

} // namespace ws
