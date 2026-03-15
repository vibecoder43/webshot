#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <userver/clients/http/client.hpp>

#include "s3_credentials_types.hpp"
#include "text.hpp"

namespace v1::s3v4 {

/**
 * @brief Canonical request and signed headers list as defined by SigV4.
 */
struct CanonicalRequestParts {
    std::string canonicalRequest;
    std::string signedHeaders; // semicolon-separated list
};

/**
 * @brief Parameters needed to compute AWS Signature V4 headers.
 */
struct [[nodiscard]] SigV4Params {
    std::string region;         // e.g. us-east-1
    std::string service = "s3"; // fixed for S3
    AccessKeyId accessKeyId;
    SecretAccessKey secretAccessKey;
    std::optional<SessionToken> sessionToken;
    std::string amzDate; // YYYYMMDDTHHMMSSZ
    std::string date;    // YYYYMMDD

    SigV4Params() = default;
    SigV4Params(
        std::string region, std::string service, const AccessKeyId &accessKeyId,
        const SecretAccessKey &secretAccessKey, std::optional<SessionToken> sessionToken,
        const std::chrono::system_clock::time_point &now
    );
};

// Utilities
[[nodiscard]] std::string buildScope(const SigV4Params &params);
[[nodiscard]] std::string
computeSignature(const SigV4Params &params, std::string_view stringToSign);
/** @return AMZ date stamp for the given time point (UTC). */
[[nodiscard]] std::string toAmzDateUtc(std::chrono::system_clock::time_point tp);
/** @return Date stamp (YYYYMMDD) for the given time point (UTC). */
[[nodiscard]] std::string toDateStampUtc(std::chrono::system_clock::time_point tp);
/** @return SHA-256 digest in hex of the input. */
[[nodiscard]] String sha256Hex(std::string_view data);

/** RFC3986 percent-encoding for AWS canonicalization. */
[[nodiscard]] String percentEncode(const String &s, bool encodeSlash);

/** Encode, sort, and join query parameters per SigV4 canonical rules. */
[[nodiscard]] std::string
canonicalizeQuery(const std::vector<std::pair<std::string, std::string>> &decoded);

/** Lowercase, insert host, and sort headers to be signed. */
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
prepareSignedHeaders(std::string host, const userver::clients::http::Headers &extra);

/** Join header names (already lowercase/sorted) with semicolons. */
[[nodiscard]] std::string buildSignedHeaders(
    const std::vector<std::pair<std::string, std::string>> &headersLowercaseTrimmedSorted
);

/** Build the canonical request string used by SigV4. */
[[nodiscard]] CanonicalRequestParts buildCanonicalRequest(
    std::string_view method, std::string_view canonicalUri,
    const std::vector<std::pair<std::string, std::string>>
        &query, // already key/value, will encode/sort
    const std::vector<std::pair<std::string, std::string>> &headersLowercaseTrimmedSorted,
    std::string_view payloadSha256Hex
);

/**
 * @brief Compute headers for an authenticated request.
 *
 * Returns the `authorization` header and auxiliary `x-amz-*` headers.
 */
[[nodiscard]] std::unordered_map<std::string, std::string> signHeaders(
    const SigV4Params &p, const String &method, const String &canonicalUri,
    const std::vector<std::pair<String, String>> &query,
    // input headers to be signed: key must be lowercase and trimmed; host must be present
    const std::vector<std::pair<String, String>> &headersLowerTrimmed,
    const String &payloadSha256Hex
);

} // namespace v1::s3v4
