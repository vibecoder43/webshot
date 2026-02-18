#include "s3/sigv4_signer.hpp"
/**
 * @file
 * @brief Helpers for AWS Signature V4 request canonicalization and signing.
 */

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>

#include <cctz/time_zone.h>

#include <userver/crypto/hash.hpp>

#include <absl/strings/ascii.h>
#include <fmt/format.h>

namespace v1::s3v4 {

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
    absl::string_view trimmed = absl::StripAsciiWhitespace(absl::string_view{s});
    std::string out;
    out.reserve(trimmed.size());
    bool in_space = false;
    for (char c : trimmed) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!in_space) {
                out.push_back(' ');
                in_space = true;
            }
        } else {
            out.push_back(c);
            in_space = false;
        }
    }
    return out;
}

/** Join header names with semicolons in order. */
std::string JoinSignedHeaders(const std::vector<std::pair<std::string, std::string>> &headers)
{
    std::ostringstream oss;
    for (std::size_t i = 0; i < headers.size(); i++) {
        if (i)
            oss << ';';
        oss << headers[i].first;
    }
    return oss.str();
}

std::string percentEncodeBytes(std::string_view s, bool encodeSlash)
{
    std::string out;
    out.reserve(s.size() * 3);
    for (char c : s) {
        if (IsUnreserved(c) || (!encodeSlash && c == '/')) {
            out.push_back(c);
        } else {
            unsigned char uc = static_cast<unsigned char>(c);
            static std::string_view kHex = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(kHex[(uc >> 4) & 0xF]);
            out.push_back(kHex[uc & 0xF]);
        }
    }
    return out;
}

std::string canonicalizeQueryImpl(const std::vector<std::pair<std::string, std::string>> &query)
{
    std::vector<std::pair<std::string, std::string>> q = query;
    for (auto &kv : q) {
        kv.first = percentEncodeBytes(kv.first, /*encodeSlash*/ true);
        kv.second = percentEncodeBytes(kv.second, /*encodeSlash*/ true);
    }
    std::sort(std::begin(q), std::end(q), [](const auto &a, const auto &b) {
        if (a.first == b.first)
            return a.second < b.second;
        return a.first < b.first;
    });
    std::ostringstream canon_query;
    for (size_t i = 0; i < q.size(); i++) {
        if (i)
            canon_query << '&';
        canon_query << q[i].first << '=' << q[i].second;
    }
    return canon_query.str();
}

std::vector<std::pair<std::string, std::string>>
toUtf8Pairs(const std::vector<std::pair<String, String>> &in)
{
    auto out = std::vector<std::pair<std::string, std::string>>{};
    out.reserve(in.size());
    for (const auto &kv : in) {
        out.emplace_back(std::string(kv.first.view()), std::string(kv.second.view()));
    }
    return out;
}

} // namespace

SigV4Params::SigV4Params(
    std::string region_, std::string service_, const AccessKeyId &accessKeyId_,
    const SecretAccessKey &secretAccessKey_, std::optional<SessionToken> sessionToken_,
    const std::chrono::system_clock::time_point &now
)
    : region(std::move(region_)), service(std::move(service_)), accessKeyId(accessKeyId_),
      secretAccessKey(secretAccessKey_), sessionToken(std::move(sessionToken_)),
      amzDate(toAmzDateUtc(now)), date(toDateStampUtc(now))
{
}

std::string buildScope(const SigV4Params &params)
{
    return fmt::format("{}/{}/{}/aws4_request", params.date, params.region, params.service);
}

std::string computeSignature(const SigV4Params &params, std::string_view string_to_sign)
{
    using userver::crypto::hash::HmacSha256;
    using userver::crypto::hash::OutputEncoding;
    auto kSecret = fmt::format("AWS4{}", params.secretAccessKey.GetUnderlying());
    auto kDate = HmacSha256(kSecret, params.date, OutputEncoding::kBinary);
    auto kRegion = HmacSha256(kDate, params.region, OutputEncoding::kBinary);
    auto kService = HmacSha256(kRegion, params.service, OutputEncoding::kBinary);
    auto kSigning = HmacSha256(kService, "aws4_request", OutputEncoding::kBinary);
    return HmacSha256(kSigning, string_to_sign, OutputEncoding::kHex);
}

std::string toAmzDateUtc(std::chrono::system_clock::time_point tp)
{
    return cctz::format("%Y%m%dT%H%M%SZ", tp, cctz::utc_time_zone());
}

std::string toDateStampUtc(std::chrono::system_clock::time_point tp)
{
    return cctz::format("%Y%m%d", tp, cctz::utc_time_zone());
}

String sha256Hex(std::string_view data)
{
    return String::fromBytesThrow(
        userver::crypto::hash::Sha256(data, userver::crypto::hash::OutputEncoding::kHex)
    );
}

String percentEncode(const String &s, bool encodeSlash)
{
    return String::fromBytesThrow(percentEncodeBytes(s.view(), encodeSlash));
}

CanonicalRequestParts buildCanonicalRequest(
    std::string_view method, std::string_view canonicalUri,
    const std::vector<std::pair<std::string, std::string>> &query,
    const std::vector<std::pair<std::string, std::string>> &headersLowercaseTrimmedSorted,
    std::string_view payloadSha256Hex
)
{
    // Canonical URI must be URI-encoded with slash preserved
    const std::string canon_uri = percentEncodeBytes(canonicalUri, /*encodeSlash*/ false);

    const std::string canon_query = canonicalizeQueryImpl(query);

    std::ostringstream canon_headers;
    for (const auto &kv : headersLowercaseTrimmedSorted)
        canon_headers << kv.first << ':' << TrimSpaces(kv.second) << "\n";
    std::string signed_headers = JoinSignedHeaders(headersLowercaseTrimmedSorted);

    std::ostringstream oss;
    oss << method << "\n"
        << canon_uri << "\n"
        << canon_query << "\n"
        << canon_headers.str() << "\n"
        << signed_headers << "\n"
        << payloadSha256Hex;

    return {oss.str(), signed_headers};
}

std::string canonicalizeQuery(const std::vector<std::pair<std::string, std::string>> &decoded)
{
    return canonicalizeQueryImpl(decoded);
}

std::vector<std::pair<std::string, std::string>>
prepareSignedHeaders(std::string host, const userver::clients::http::Headers &extra)
{
    std::vector<std::pair<std::string, std::string>> v;
    v.reserve(extra.size() + 1);
    v.emplace_back("host", std::move(host));
    for (const auto &kv : extra) {
        std::string keyLower;
        keyLower.reserve(kv.first.size());
        for (char c : kv.first)
            keyLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        v.emplace_back(std::move(keyLower), kv.second);
    }
    std::sort(std::begin(v), std::end(v), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
    return v;
}

std::string buildSignedHeaders(
    const std::vector<std::pair<std::string, std::string>> &headersLowercaseTrimmedSorted
)
{
    return JoinSignedHeaders(headersLowercaseTrimmedSorted);
}

std::unordered_map<std::string, std::string> signHeaders(
    const SigV4Params &p, const String &method, const String &canonicalUri,
    const std::vector<std::pair<String, String>> &query,
    const std::vector<std::pair<String, String>> &headersLowerTrimmed,
    const String &payloadSha256Hex
)
{
    auto queryUtf8 = toUtf8Pairs(query);
    auto headersUtf8 = toUtf8Pairs(headersLowerTrimmed);
    auto headers = headersUtf8;
    auto out = std::unordered_map<std::string, std::string>{};

    auto payloadHex = std::string(payloadSha256Hex.view());
    out["x-amz-date"] = p.amzDate;
    out["x-amz-content-sha256"] = payloadHex;
    if (p.sessionToken)
        out["x-amz-security-token"] = std::string(p.sessionToken->GetUnderlying().view());
    for (auto &&kv : out)
        headers.emplace_back(kv.first, kv.second);
    std::sort(std::begin(headers), std::end(headers), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });

    const auto cr = buildCanonicalRequest(
        method.view(), canonicalUri.view(), queryUtf8, headers, payloadHex
    );

    auto scope = buildScope(p);
    auto string_to_sign = fmt::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", p.amzDate, scope, sha256Hex(cr.canonicalRequest)
    );

    auto signature = computeSignature(p, string_to_sign);

    auto credential = fmt::format("{}/{}", p.accessKeyId.GetUnderlying(), scope);
    auto authorization = fmt::format(
        "AWS4-HMAC-SHA256 Credential={}, SignedHeaders={}, Signature={}", credential,
        cr.signedHeaders, signature
    );

    out["authorization"] = std::move(authorization);
    return out;
}

} // namespace v1::s3v4
