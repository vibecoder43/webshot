#include "s3/sigv4_signer.hpp"
/**
 * @file
 * @brief Helpers for AWS Signature  request canonicalization and signing.
 */

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>

#include <cctz/time_zone.h>

#include "integers.hpp"

#include <userver/crypto/hash.hpp>

#include <absl/strings/ascii.h>

namespace ws::s3 {

namespace us = userver;
namespace httpc = us::clients::http;

namespace {

/** Characters that do not require percent-encoding. */
inline bool IsUnreserved(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

/** Collapse runs of spaces and trim at both ends. */
std::string TrimSpaces(const std::string &s)
{
    auto out = s;
    absl::RemoveExtraAsciiWhitespace(&out);
    return out;
}

/** Join header names with semicolons in order. */
std::string JoinSignedHeaders(const std::vector<std::pair<std::string, std::string>> &headers)
{
    std::string out;
    bool is_first = true;
    for (const auto &name : headers | std::views::keys) {
        if (!is_first)
            out.push_back(';');
        is_first = false;
        out += name;
    }
    return out;
}

// doesn't have an ada:: equivalent
std::string PercentEncodeBytes(std::string_view s, EncodeSlash encode_slash)
{
    std::string out;
    out.reserve(Raw(unsize(s) * 3_u64));
    for (char c : s) {
        if (IsUnreserved(c) || (encode_slash == EncodeSlash::kNo && c == '/')) {
            out.push_back(c);
        } else {
            unsigned char uc = static_cast<unsigned char>(c);
            static std::string_view hex = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(hex[(uc >> 4) & 0xF]);
            out.push_back(hex[uc & 0xF]);
        }
    }
    return out;
}

std::string CanonicalizeQueryImpl(const std::vector<std::pair<std::string, std::string>> &query)
{
    auto q = query;
    for (auto &[k, v] : q) {
        k = PercentEncodeBytes(k, EncodeSlash::kYes);
        v = PercentEncodeBytes(v, EncodeSlash::kYes);
    }
    std::ranges::sort(q);
    std::string out;
    bool is_first = true;
    for (const auto &[name, value] : q) {
        if (!is_first)
            out.push_back('&');
        is_first = false;
        out += name;
        out.push_back('=');
        out += value;
    }
    return out;
}

} // namespace

SigParams::SigParams(
    std::string region, std::string service, const AccessKeyId &access_key_id,
    const SecretAccessKey &secret_access_key, std::optional<SessionToken> session_token,
    const std::chrono::system_clock::time_point &now
)
    : region(std::move(region)), service(std::move(service)), access_key_id(access_key_id),
      secret_access_key(secret_access_key), session_token(std::move(session_token)),
      amz_date(ToAmzDateUtc(now)), date(ToDateStampUtc(now))
{
}

std::string MakeScope(const SigParams &params)
{
    return std::format("{}/{}/{}/aws4_request", params.date, params.region, params.service);
}

std::string ComputeSignature(const SigParams &params, std::string_view string_to_sign)
{
    using us::crypto::hash::HmacSha256;
    using us::crypto::hash::OutputEncoding;
    auto secret = std::format("AWS4{}", params.secret_access_key.GetUnderlying());
    auto date = HmacSha256(secret, params.date, OutputEncoding::kBinary);
    auto region = HmacSha256(date, params.region, OutputEncoding::kBinary);
    auto service = HmacSha256(region, params.service, OutputEncoding::kBinary);
    auto signing = HmacSha256(service, "aws4_request", OutputEncoding::kBinary);
    return HmacSha256(signing, string_to_sign, OutputEncoding::kHex);
}

std::string ToAmzDateUtc(std::chrono::system_clock::time_point tp)
{
    return cctz::format("%Y%m%dT%H%M%SZ", tp, cctz::utc_time_zone());
}

std::string ToDateStampUtc(std::chrono::system_clock::time_point tp)
{
    return cctz::format("%Y%m%d", tp, cctz::utc_time_zone());
}

String Sha256Hex(std::string_view data)
{
    return *String::FromBytes(
        us::crypto::hash::Sha256(data, us::crypto::hash::OutputEncoding::kHex)
    );
}

String PercentEncode(const String &s, EncodeSlash encode_slash)
{
    return *String::FromBytes(PercentEncodeBytes(s.View(), encode_slash));
}

CanonicalRequestParts MakeCanonicalRequest(
    std::string_view method, std::string_view canonical_uri,
    const std::vector<std::pair<std::string, std::string>> &query,
    const std::vector<std::pair<std::string, std::string>> &headers_lowercase_trimmed_sorted,
    std::string_view payload_sha256_hex
)
{
    // Canonical URI must be URI-encoded with slash preserved
    auto canonical_uri_encoded = PercentEncodeBytes(canonical_uri, EncodeSlash::kNo);

    auto canonical_query = CanonicalizeQueryImpl(query);

    std::string canonical_headers;
    for (const auto &[k, v] : headers_lowercase_trimmed_sorted) {
        canonical_headers += k;
        canonical_headers.push_back(':');
        canonical_headers += TrimSpaces(v);
        canonical_headers.push_back('\n');
    }
    auto signed_headers = JoinSignedHeaders(headers_lowercase_trimmed_sorted);

    auto canonical_request = std::format(
        "{}\n{}\n{}\n{}\n{}\n{}", method, canonical_uri_encoded, canonical_query, canonical_headers,
        signed_headers, payload_sha256_hex
    );

    return {
        .canonical_request = std::move(canonical_request),
        .signed_headers = std::move(signed_headers),
    };
}

std::string CanonicalizeQuery(const std::vector<std::pair<std::string, std::string>> &decoded)
{
    return CanonicalizeQueryImpl(decoded);
}

std::vector<std::pair<std::string, std::string>>
PrepareSignedHeaders(std::string host, const httpc::Headers &extra)
{
    std::vector<std::pair<std::string, std::string>> v;
    v.reserve(NumericCast<size_t>(ssize(extra) + 1_i64));
    v.emplace_back("host", std::move(host));
    for (const auto &[name, value] : extra) {
        v.emplace_back(absl::AsciiStrToLower(std::string_view{name}), value);
    }
    std::ranges::sort(v, {}, &std::pair<std::string, std::string>::first);
    return v;
}

std::string MakeSignedHeaders(
    const std::vector<std::pair<std::string, std::string>> &headers_lowercase_trimmed_sorted
)
{
    return JoinSignedHeaders(headers_lowercase_trimmed_sorted);
}

std::unordered_map<std::string, std::string> SignHeaders(
    const SigParams &p, const String &method, const String &canonical_uri,
    const std::vector<std::pair<String, String>> &query,
    const std::vector<std::pair<String, String>> &headers_lower_trimmed,
    const String &payload_sha256_hex
)
{
    auto query_utf8 = text::ToBytesPairs(query);
    auto headers_utf8 = text::ToBytesPairs(headers_lower_trimmed);
    auto headers = headers_utf8;
    std::unordered_map<std::string, std::string> out{};

    auto payload_hex = payload_sha256_hex.ToBytes();
    out["x-amz-date"] = p.amz_date;
    out["x-amz-content-sha256"] = payload_hex;
    if (p.session_token)
        out["x-amz-security-token"] = p.session_token->GetUnderlying().ToBytes();
    for (const auto &[name, value] : out)
        headers.emplace_back(name, value);
    std::ranges::sort(headers, {}, &std::pair<std::string, std::string>::first);

    const auto cr = MakeCanonicalRequest(
        method.View(), canonical_uri.View(), query_utf8, headers, payload_hex
    );

    auto scope = MakeScope(p);
    auto string_to_sign = std::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", p.amz_date, scope, Sha256Hex(cr.canonical_request)
    );

    auto signature = ComputeSignature(p, string_to_sign);

    auto credential = std::format("{}/{}", p.access_key_id.GetUnderlying(), scope);
    auto authorization = std::format(
        "AWS4-HMAC-SHA256 Credential={}, SignedHeaders={}, Signature={}", credential,
        cr.signed_headers, signature
    );

    out["authorization"] = std::move(authorization);
    return out;
}

} // namespace ws::s3
