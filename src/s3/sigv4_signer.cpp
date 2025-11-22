#include "s3/sigv4_signer.hpp"
/**
 * @file
 * @brief Helpers for AWS Signature V4 request canonicalization and signing.
 */

#include <algorithm>
#include <cctype>
#include <sstream>

#include <cctz/time_zone.h>

#include <userver/crypto/hash.hpp>

#include <absl/strings/ascii.h>
#include <fmt/format.h>

namespace v1::s3v4 {

namespace {

/** Characters that do not require percent‑encoding. */
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
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (i)
            oss << ';';
        oss << headers[i].first;
    }
    return oss.str();
}

} // namespace

std::string ToAmzDateUtc(std::chrono::system_clock::time_point tp)
{
    const auto tz = cctz::utc_time_zone();
    return cctz::format("%Y%m%dT%H%M%SZ", tp, tz);
}

std::string ToDateStampUtc(std::chrono::system_clock::time_point tp)
{
    const auto tz = cctz::utc_time_zone();
    return cctz::format("%Y%m%d", tp, tz);
}

std::string Sha256Hex(std::string_view data)
{
    return USERVER_NAMESPACE::crypto::hash::Sha256(
        data, USERVER_NAMESPACE::crypto::hash::OutputEncoding::kHex
    );
}

std::string PercentEncode(std::string_view s, bool encodeSlash)
{
    std::string out;
    out.reserve(s.size() * 3);
    for (char c : s) {
        if (IsUnreserved(c) || (!encodeSlash && c == '/')) {
            out.push_back(c);
        } else {
            unsigned char uc = static_cast<unsigned char>(c);
            static const char *kHex = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(kHex[(uc >> 4) & 0xF]);
            out.push_back(kHex[uc & 0xF]);
        }
    }
    return out;
}

CanonicalRequestParts BuildCanonicalRequest(
    std::string_view method, std::string_view canonicalUri,
    const std::vector<std::pair<std::string, std::string>> &query,
    const std::vector<std::pair<std::string, std::string>> &headersLowercaseTrimmedSorted,
    std::string_view payloadSha256Hex
)
{
    // Canonical URI must be URI-encoded with slash preserved
    const std::string canon_uri = PercentEncode(canonicalUri, /*encodeSlash*/ false);

    std::vector<std::pair<std::string, std::string>> q = query;
    for (auto &kv : q) {
        kv.first = PercentEncode(kv.first, /*encodeSlash*/ true);
        kv.second = PercentEncode(kv.second, /*encodeSlash*/ true);
    }
    std::sort(q.begin(), q.end(), [](const auto &a, const auto &b) {
        if (a.first == b.first)
            return a.second < b.second;
        return a.first < b.first;
    });
    std::ostringstream canon_query;
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (i)
            canon_query << '&';
        canon_query << q[i].first << '=' << q[i].second;
    }

    std::ostringstream canon_headers;
    for (const auto &kv : headersLowercaseTrimmedSorted) {
        canon_headers << kv.first << ':' << TrimSpaces(kv.second) << "\n";
    }
    std::string signed_headers = JoinSignedHeaders(headersLowercaseTrimmedSorted);

    std::ostringstream oss;
    oss << method << "\n"
        << canon_uri << "\n"
        << canon_query.str() << "\n"
        << canon_headers.str() << "\n"
        << signed_headers << "\n"
        << payloadSha256Hex;

    return {oss.str(), signed_headers};
}

std::unordered_map<std::string, std::string> SignHeaders(
    const SigV4Params &p, std::string_view method, std::string_view canonicalUri,
    const std::vector<std::pair<std::string, std::string>> &query,
    const std::vector<std::pair<std::string, std::string>> &headersLowerTrimmed,
    std::string_view payloadSha256Hex
)
{
    std::vector<std::pair<std::string, std::string>> headers = headersLowerTrimmed;
    // Add required headers
    std::unordered_map<std::string, std::string> out;
    out["x-amz-date"] = p.amzDate;
    out["x-amz-content-sha256"] = std::string(payloadSha256Hex);
    if (p.sessionToken)
        out["x-amz-security-token"] = p.sessionToken->GetUnderlying();
    // Merge to headers vector for canonicalization and sort
    for (const auto &kv : out)
        headers.emplace_back(kv.first, kv.second);
    std::sort(headers.begin(), headers.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });

    const auto cr = BuildCanonicalRequest(method, canonicalUri, query, headers, payloadSha256Hex);

    const std::string scope = fmt::format("{}/{}/{}/aws4_request", p.date, p.region, p.service);
    const std::string string_to_sign = fmt::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", p.amzDate, scope, Sha256Hex(cr.canonicalRequest)
    );

    namespace US = USERVER_NAMESPACE::crypto::hash;
    const std::string kSecret = fmt::format("AWS4{}", p.secretAccessKey.GetUnderlying());
    const std::string kDate = US::HmacSha256(kSecret, p.date, US::OutputEncoding::kBinary);
    const std::string kRegion = US::HmacSha256(kDate, p.region, US::OutputEncoding::kBinary);
    const std::string kService = US::HmacSha256(kRegion, p.service, US::OutputEncoding::kBinary);
    const std::string kSigning = US::HmacSha256(
        kService, "aws4_request", US::OutputEncoding::kBinary
    );
    const std::string signature = US::HmacSha256(
        kSigning, string_to_sign, US::OutputEncoding::kHex
    );

    const std::string credential = fmt::format("{}/{}", p.accessKeyId.GetUnderlying(), scope);
    std::string authorization = fmt::format(
        "AWS4-HMAC-SHA256 Credential={}, SignedHeaders={}, Signature={}", credential,
        cr.signedHeaders, signature
    );

    out["authorization"] = std::move(authorization);
    return out;
}

} // namespace v1::s3v4
