#pragma once

/**
 * @file
 * @brief Helpers for time conversions and opaque pagination tokens.
 *
 * Provides microsecond conversions for time points and generic helpers to
 * encode/decode DTOs into Base64-url JSON tokens used as page cursors.
 */
#include "crypto.hpp"
#include "json.hpp"
#include "text.hpp"
#include "try.hpp"

#include <chrono>
#include <optional>

#include <userver/crypto/base64.hpp>
#include <userver/formats/json.hpp>

namespace ws::crud {

namespace us = userver;
namespace json = us::formats::json;
/** @brief Clock type used for pagination cursors. */
using Clock = std::chrono::system_clock;

enum class PageDirection {
    kNext,
    kPrevious,
};

/**
 * @brief Convert a time point to microseconds since Unix epoch.
 *
 * The microsecond value is used in serialized pagination cursors.
 */
[[nodiscard]] int64_t TimePointToMicros(Clock::time_point tp);

/**
 * @brief Convert microseconds since Unix epoch back to a time point.
 *
 * Inverse of timePointToMicros for values produced by that helper.
 */
[[nodiscard]] Clock::time_point MicrosToTimePoint(int64_t micros);

/**
 * @brief Decode a Base64-url JSON token into a DTO.
 *
 * @tparam Dto DTO type with JSON parse/serialize support generated from the schema.
 * @param token Base64-url encoded JSON document.
 * @return Parsed DTO value, or empty optional on malformed input.
 */
template <typename Dto> [[nodiscard]] std::optional<Dto> DecodeToken(const String &token)
{
    const auto decoded = TRY(ws::crypto::Base64UrlDecode(token.View(), false));
    const auto decoded_text = TRY(String::FromBytes(decoded));
    return TRY(ws::json::Parse<Dto>(decoded_text, false));
}

/**
 * @brief Encode a DTO as a Base64-url JSON token.
 *
 * Tokens are emitted without padding so they can be used safely in URLs.
 */
template <typename Dto> [[nodiscard]] String EncodeToken(const Dto &dto)
{
    return String::FromBytes(
               us::crypto::base64::Base64UrlEncode(
                   json::ToString(json::ValueBuilder(dto).ExtractValue()),
                   us::crypto::base64::Pad::kWithout
               )
    )
        .Expect();
}

} // namespace ws::crud
