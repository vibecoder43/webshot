#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "s3_credentials_types.hpp"

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
};

// Utilities
/** @return AMZ date stamp for the given time point (UTC). */
[[nodiscard]] std::string ToAmzDateUtc(std::chrono::system_clock::time_point tp);
/** @return Date stamp (YYYYMMDD) for the given time point (UTC). */
[[nodiscard]] std::string ToDateStampUtc(std::chrono::system_clock::time_point tp);
/** @return SHA-256 digest in hex of the input. */
[[nodiscard]] std::string Sha256Hex(std::string_view data);

/** RFC3986 percent‑encoding for AWS canonicalization. */
[[nodiscard]] std::string PercentEncode(std::string_view s, bool encodeSlash);

/** Build the canonical request string used by SigV4. */
[[nodiscard]] CanonicalRequestParts BuildCanonicalRequest(
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
[[nodiscard]] std::unordered_map<std::string, std::string> SignHeaders(
    const SigV4Params &p, std::string_view method, std::string_view canonicalUri,
    const std::vector<std::pair<std::string, std::string>> &query,
    // input headers to be signed: key must be lowercase and trimmed; host must be present
    const std::vector<std::pair<std::string, std::string>> &headersLowerTrimmed,
    std::string_view payloadSha256Hex
);

} // namespace v1::s3v4
