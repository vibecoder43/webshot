#pragma once

#include "expected.hpp"
#include "integers.hpp"
#include "url.hpp"

#include <string>

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
 * @brief Parsed and normalized link used at REST and denylist boundaries.
 *
 * This type encapsulates request-facing link normalization semantics.
 * Internal URL manipulation code should use Url directly.
 *
 * Invariants after construction via fromText:
 * - Scheme is http or https (defaulted to http when absent).
 * - Username/password are cleared; fragment/hash is stripped.
 * - Port is stripped.
 * - Host is lower-cased, validated, and trailing dot removed; IP literals are rejected.
 * - Path/query are normalized; total URL byte length is limited by the caller.
 * - A scheme-less, trailing-slash-trimmed form is stored for lookups.
 */
struct [[nodiscard]] Link {
    Url url;

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
     * @return Normalized Link.
     */
    [[nodiscard]] static Expected<Link, LinkError> fromText(const String &text, usize urlBytesMax);

    /** @return Normalized, lower-cased host, punycode if applicable. */
    [[nodiscard]] String host() const;
    /** @return Full URL using the http scheme. */
    [[nodiscard]] String httpUrl() const;
    [[nodiscard]] String httpsUrl() const;
    /** @return Scheme-less normalized representation used for lookups. */
    [[nodiscard]] String normalized() const;
};

} // namespace v1
