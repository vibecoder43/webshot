#pragma once

#include "expected.hpp"
#include "integers.hpp"
#include "url.hpp"

#include <string>
#include <type_traits>

#include "text.hpp"

namespace v1 {

struct [[nodiscard]] LinkError final {
    enum class Code {
        kMissingScheme,
        kUnsupportedScheme,
        kFailedToParse,
        kMissingHostname,
        kIpAddressNotAllowed,
        kInvalidHost,
        kUrlTooLong,
    };
    Code code;
};

/**
 * @brief Parsed and normalized URL used as an index key.
 *
 * This type encapsulates a normalized representation of a link.
 *
 * Invariants after construction via fromText:
 * - Scheme is http or https (defaulted to http when absent).
 * - Username/password are cleared; fragment/hash is stripped.
 * - Host is lower-cased, validated, and trailing dot removed; IP literals are rejected.
 * - Path/query are normalized; total URL byte length is limited by the caller.
 * - A scheme-less, trailing-slash-trimmed form is stored for lookups.
 */
struct [[nodiscard]] Link {
    Url url;

    enum class FromTextOptions {
        kNone = 0,
        kStripPort = 1 << 0,
        kStripQuery = 1 << 1,
    };

    friend constexpr FromTextOptions operator|(FromTextOptions lhs, FromTextOptions rhs) noexcept
    {
        using U = std::underlying_type_t<FromTextOptions>;
        return static_cast<FromTextOptions>(static_cast<U>(lhs) | static_cast<U>(rhs));
    }

    friend constexpr FromTextOptions operator&(FromTextOptions lhs, FromTextOptions rhs) noexcept
    {
        using U = std::underlying_type_t<FromTextOptions>;
        return static_cast<FromTextOptions>(static_cast<U>(lhs) & static_cast<U>(rhs));
    }

    static constexpr bool hasOption(FromTextOptions options, FromTextOptions flag) noexcept
    {
        using U = std::underlying_type_t<FromTextOptions>;
        return static_cast<U>(options & flag) != 0;
    }

    /**
     * @brief Construct a Link from normalized UTF-8 text.
     *
     * Accepts text that was already validated and normalized by String;
     * performs trimming, default scheme insertion (http), validation of scheme
     * and host, punycode handling, clears credentials and fragment, and
     * enforces a limit on the total URL length in bytes.
     *
     * @param text Prevalidated, normalized UTF-8 text.
     * @param urlBytesMax Maximum allowed URL length in bytes.
     * @param options Extra normalization options (for example strip port or query).
     * @return Normalized Link.
     */
    [[nodiscard]] static Expected<Link, LinkError>
    fromText(const String &text, usize urlBytesMax, FromTextOptions options);

    /** @return Normalized, lower-cased host, punycode if applicable. */
    [[nodiscard]] String host() const;
    /** @return Full URL using the http scheme. */
    [[nodiscard]] String httpUrl() const;
    [[nodiscard]] String httpsUrl() const;
    /** @return Scheme-less normalized representation used for lookups. */
    [[nodiscard]] String normalized() const;
};

} // namespace v1
