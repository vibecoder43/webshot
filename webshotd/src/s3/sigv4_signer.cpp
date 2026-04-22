#include "s3/sigv4_signer.hpp"
/**
 * @file
 * @brief Helpers for AWS Signature V4 request canonicalization and signing.
 */

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>

#include <cctz/time_zone.h>

#include "integers.hpp"

#include <userver/crypto/hash.hpp>

#include <absl/strings/ascii.h>

namespace v1::s3v4 {

namespace {

/** Characters that do not require percent-encoding. */
inline bool isUnreserved(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

/** Collapse runs of spaces and trim at both ends. */
std::string trimSpaces(const std::string &s)
{
    auto out = s;
    absl::RemoveExtraAsciiWhitespace(&out);
    return out;
}

/** Join header names with semicolons in order. */
std::string joinSignedHeaders(const std::vector<std::pair<std::string, std::string>> &headers)
{
    std::string out;
    bool isFirst = true;
    for (const auto &name : headers | std::views::keys) {
        if (!isFirst)
            out.push_back(';');
        isFirst = false;
        out += name;
    }
    return out;
}

std::string percentEncodeBytes(std::string_view s, EncodeSlash encodeSlash)
{
    std::string out;
    out.reserve(numericCast<size_t>(ssize(s) * 3_i64));
    for (char c : s) {
        if (isUnreserved(c) || (encodeSlash == EncodeSlash::kNo && c == '/')) {
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
    auto q = query;
    for (auto &[k, v] : q) {
        k = percentEncodeBytes(k, EncodeSlash::kYes);
        v = percentEncodeBytes(v, EncodeSlash::kYes);
    }
    std::ranges::sort(q);
    std::string out;
    bool isFirst = true;
    for (const auto &[name, value] : q) {
        if (!isFirst)
            out.push_back('&');
        isFirst = false;
        out += name;
        out.push_back('=');
        out += value;
    }
    return out;
}

std::vector<std::pair<std::string, std::string>>
toUtf8Pairs(const std::vector<std::pair<String, String>> &in)
{
    std::vector<std::pair<std::string, std::string>> out{};
    out.reserve(in.size());
    for (const auto &[k, v] : in) {
        out.emplace_back(std::string{k.view()}, std::string{v.view()});
    }
    return out;
}

} // namespace

SigV4Params::SigV4Params(
    std::string region, std::string service, const AccessKeyId &accessKeyId,
    const SecretAccessKey &secretAccessKey, std::optional<SessionToken> sessionToken,
    const std::chrono::system_clock::time_point &now
)
    : region(std::move(region)), service(std::move(service)), accessKeyId(accessKeyId),
      secretAccessKey(secretAccessKey), sessionToken(std::move(sessionToken)),
      amzDate(toAmzDateUtc(now)), date(toDateStampUtc(now))
{
}

std::string buildScope(const SigV4Params &params)
{
    return std::format("{}/{}/{}/aws4_request", params.date, params.region, params.service);
}

std::string computeSignature(const SigV4Params &params, std::string_view stringToSign)
{
    using us::crypto::hash::HmacSha256;
    using us::crypto::hash::OutputEncoding;
    auto kSecret = std::format("AWS4{}", params.secretAccessKey.GetUnderlying());
    auto kDate = HmacSha256(kSecret, params.date, OutputEncoding::kBinary);
    auto kRegion = HmacSha256(kDate, params.region, OutputEncoding::kBinary);
    auto kService = HmacSha256(kRegion, params.service, OutputEncoding::kBinary);
    auto kSigning = HmacSha256(kService, "aws4_request", OutputEncoding::kBinary);
    return HmacSha256(kSigning, stringToSign, OutputEncoding::kHex);
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
    return String::fromBytes(us::crypto::hash::Sha256(data, us::crypto::hash::OutputEncoding::kHex))
        .expect();
}

String percentEncode(const String &s, EncodeSlash encodeSlash)
{
    return String::fromBytes(percentEncodeBytes(s.view(), encodeSlash)).expect();
}

CanonicalRequestParts buildCanonicalRequest(
    std::string_view method, std::string_view canonicalUri,
    const std::vector<std::pair<std::string, std::string>> &query,
    const std::vector<std::pair<std::string, std::string>> &headersLowercaseTrimmedSorted,
    std::string_view payloadSha256Hex
)
{
    // Canonical URI must be URI-encoded with slash preserved
    const auto canonicalUriEncoded = percentEncodeBytes(canonicalUri, EncodeSlash::kNo);

    const auto canonicalQuery = canonicalizeQueryImpl(query);

    std::string canonicalHeaders;
    for (const auto &[k, v] : headersLowercaseTrimmedSorted) {
        canonicalHeaders += k;
        canonicalHeaders.push_back(':');
        canonicalHeaders += trimSpaces(v);
        canonicalHeaders.push_back('\n');
    }
    auto signedHeaders = joinSignedHeaders(headersLowercaseTrimmedSorted);

    auto canonicalRequest = std::format(
        "{}\n{}\n{}\n{}\n{}\n{}", method, canonicalUriEncoded, canonicalQuery, canonicalHeaders,
        signedHeaders, payloadSha256Hex
    );

    return {
        .canonicalRequest = std::move(canonicalRequest),
        .signedHeaders = std::move(signedHeaders),
    };
}

std::string canonicalizeQuery(const std::vector<std::pair<std::string, std::string>> &decoded)
{
    return canonicalizeQueryImpl(decoded);
}

std::vector<std::pair<std::string, std::string>>
prepareSignedHeaders(std::string host, const httpc::Headers &extra)
{
    std::vector<std::pair<std::string, std::string>> v;
    v.reserve(numericCast<size_t>(ssize(extra) + 1_i64));
    v.emplace_back("host", std::move(host));
    for (const auto &[name, value] : extra) {
        v.emplace_back(absl::AsciiStrToLower(std::string_view{name}), value);
    }
    std::ranges::sort(v, {}, &std::pair<std::string, std::string>::first);
    return v;
}

std::string buildSignedHeaders(
    const std::vector<std::pair<std::string, std::string>> &headersLowercaseTrimmedSorted
)
{
    return joinSignedHeaders(headersLowercaseTrimmedSorted);
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
    std::unordered_map<std::string, std::string> out{};

    auto payloadHex = std::to_string(payloadSha256Hex);
    out["x-amz-date"] = p.amzDate;
    out["x-amz-content-sha256"] = payloadHex;
    if (p.sessionToken)
        out["x-amz-security-token"] = std::to_string(p.sessionToken->GetUnderlying());
    for (const auto &[name, value] : out)
        headers.emplace_back(name, value);
    std::ranges::sort(headers, {}, &std::pair<std::string, std::string>::first);

    const auto cr = buildCanonicalRequest(
        method.view(), canonicalUri.view(), queryUtf8, headers, payloadHex
    );

    auto scope = buildScope(p);
    auto stringToSign = std::format(
        "AWS4-HMAC-SHA256\n{}\n{}\n{}", p.amzDate, scope, sha256Hex(cr.canonicalRequest)
    );

    auto signature = computeSignature(p, stringToSign);

    auto credential = std::format("{}/{}", p.accessKeyId.GetUnderlying(), scope);
    auto authorization = std::format(
        "AWS4-HMAC-SHA256 Credential={}, SignedHeaders={}, Signature={}", credential,
        cr.signedHeaders, signature
    );

    out["authorization"] = std::move(authorization);
    return out;
}

} // namespace v1::s3v4
