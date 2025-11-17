#pragma once

/**
 * @file
 * @brief Helpers for time conversions and opaque pagination tokens.
 *
 * Provides microsecond conversions for time points and generic helpers to
 * encode/decode DTOs into Base64-url JSON tokens used as pagination cursors.
 */

#include <chrono>
#include <exception>
#include <optional>
#include <string>

#include <userver/crypto/base64.hpp>
#include <userver/formats/json.hpp>

namespace v1::crud {

/** @brief Clock type used for pagination cursors. */
using Clock = std::chrono::system_clock;

/**
 * @brief Convert a time point to microseconds since Unix epoch.
 *
 * The microsecond value is used in serialized pagination cursors.
 */
[[nodiscard]] int64_t timePointToMicros(Clock::time_point tp);

/**
 * @brief Convert microseconds since Unix epoch back to a time point.
 *
 * Inverse of timePointToMicros for values produced by that helper.
 */
[[nodiscard]] Clock::time_point microsToTimePoint(int64_t micros);

/**
 * @brief Decode a Base64-url JSON token into a DTO.
 *
 * @tparam Dto DTO type with JSON parse/serialize support generated from the schema.
 * @param token Base64-url encoded JSON document.
 * @return Parsed DTO value, or empty optional on malformed input.
 */
template <typename Dto> [[nodiscard]] std::optional<Dto> decodeToken(const std::string &token)
{
    try {
        const auto decoded = userver::crypto::base64::Base64UrlDecode(token);
        const auto val = userver::formats::json::FromString(decoded);
        return val.As<Dto>();
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

/**
 * @brief Encode a DTO as a Base64-url JSON token.
 *
 * Tokens are emitted without padding so they can be used safely in URLs.
 */
template <typename Dto> [[nodiscard]] std::string encodeToken(const Dto &dto)
{
    return userver::crypto::base64::Base64UrlEncode(
        userver::formats::json::ToString(userver::formats::json::ValueBuilder(dto).ExtractValue()),
        userver::crypto::base64::Pad::kWithout
    );
}

} // namespace v1::crud
