#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <userver/clients/http/response.hpp>

#include "s3/credentials_types.hpp"
#include "text.hpp"

namespace ws::s3 {

namespace us = userver;
namespace httpc = us::clients::http;
enum class EncodeSlash {
    kNo,
    kYes,
};

/**
 * @brief Canonical request and signed headers list as defined by Sig.
 */
struct CanonicalRequestParts {
    std::string canonical_request;
    std::string signed_headers; // semicolon-separated list
};

/**
 * @brief Parameters needed to compute AWS Signature  headers.
 */
struct [[nodiscard]] SigParams {
    std::string region;         // e.g. us-east-1
    std::string service = "s3"; // fixed for
    AccessKeyId access_key_id;
    SecretAccessKey secret_access_key;
    std::optional<SessionToken> session_token;
    std::string amz_date; // YYYYMMDDTHHMMSSZ
    std::string date;     // YYYYMMDD

    SigParams() = default;
    SigParams(
        std::string region, std::string service, const AccessKeyId &access_key_id,
        const SecretAccessKey &secret_access_key, std::optional<SessionToken> session_token,
        const std::chrono::system_clock::time_point &now
    );
};

// Utilities
[[nodiscard]] std::string BuildScope(const SigParams &params);
[[nodiscard]] std::string
ComputeSignature(const SigParams &params, std::string_view string_to_sign);
/** @return AMZ date stamp for the given time point (UTC). */
[[nodiscard]] std::string ToAmzDateUtc(std::chrono::system_clock::time_point tp);
/** @return Date stamp (YYYYMMDD) for the given time point (UTC). */
[[nodiscard]] std::string ToDateStampUtc(std::chrono::system_clock::time_point tp);
/** @return SHA-256 digest in hex of the input. */
[[nodiscard]] String Sha256Hex(std::string_view data);

/** RFC3986 percent-encoding for AWS canonicalization. */
[[nodiscard]] String PercentEncode(const String &s, EncodeSlash encode_slash);

/** Encode, sort, and join query parameters per Sig canonical rules. */
[[nodiscard]] std::string
CanonicalizeQuery(const std::vector<std::pair<std::string, std::string>> &decoded);

/** Lowercase, insert host, and sort headers to be signed. */
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
PrepareSignedHeaders(std::string host, const httpc::Headers &extra);

/** Join header names (already lowercase/sorted) with semicolons. */
[[nodiscard]] std::string BuildSignedHeaders(
    const std::vector<std::pair<std::string, std::string>> &headers_lowercase_trimmed_sorted
);

/** Build the canonical request string used by Sig. */
[[nodiscard]] CanonicalRequestParts BuildCanonicalRequest(
    std::string_view method, std::string_view canonical_uri,
    const std::vector<std::pair<std::string, std::string>>
        &query, // already key/value, will encode/sort
    const std::vector<std::pair<std::string, std::string>> &headers_lowercase_trimmed_sorted,
    std::string_view payload_Sha256Hex
);

/**
 * @brief Compute headers for an authenticated request.
 *
 * Returns the `authorization` header and auxiliary `x-amz-*` headers.
 */
[[nodiscard]] std::unordered_map<std::string, std::string> SignHeaders(
    const SigParams &p, const String &method, const String &canonical_uri,
    const std::vector<std::pair<String, String>> &query,
    // input headers to be signed: key must be lowercase and trimmed; host must be present
    const std::vector<std::pair<String, String>> &headers_lower_trimmed,
    const String &payload_Sha256Hex
);

} // namespace ws::s3
