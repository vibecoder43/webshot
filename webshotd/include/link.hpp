#pragma once

#include "expected.hpp"
#include "integers.hpp"
#include "url.hpp"

#include "text.hpp"

namespace ws {

struct [[nodiscard]] LinkError final {
    enum class Code {
        kMissingScheme,
        kUnsupportedScheme,
        kFailedToParse,
        kMissingHostname,
        kIpAddressNotAllowed,
        kInvalidHost,
        kInputTooLong,
        kNormalizedHrefTooLong,
    };
    Code code;
};

/**
 * @brief Parsed and normalized link used at REST and access-policy boundaries.
 *
 * This type encapsulates request-facing link normalization semantics.
 * Internal URL manipulation code should use Url directly.
 *
 * Invariants after construction via FromText:
 * - Scheme is http or https, scheme-less input is accepted and normalized.
 * - Username, password are cleared; fragment, hash are stripped.
 * - Port is stripped.
 * - Host is lower-cased, validated, and trailing dot removed; IP literals are rejected.
 * - Path, query are normalized; total URL byte length is limited by the caller.
 * - A scheme-less, trailing-slash-trimmed form is stored for lookups.
 */
class [[nodiscard]] Link {
public:
    /**
     * @brief Construct a Link from normalized UTF-8 text.
     *
     * Accepts text that was already validated and normalized by String;
     * performs trimming, default scheme insertion for parsing, validation of
     * scheme and host, punycode handling, clears credentials and fragment, and
     * enforces the configured URL byte limit accepted when parsing a Link.
     *
     * @param text Prevalidated, normalized UTF-8 text.
     * @param url_bytes_max Maximum URL byte length accepted when parsing a Link.
     * @return Normalized Link.
     */
    [[nodiscard]] static Expected<Link, LinkError>
    FromText(const String &text, usize url_bytes_max);

    /**
     * @brief Construct a Link from a parsed Url.
     *
     * Enforces the same invariants as FromText, including byte-length limit
     * on the resulting normalized href.
     */
    [[nodiscard]] static Expected<Link, LinkError> FromUrl(const Url &url, usize url_bytes_max);

    /** @return Normalized, lower-cased hostname, punycode if applicable. */
    [[nodiscard]] String Hostname() const;
    /** @return Normalized pathname component. */
    [[nodiscard]] String Pathname() const;
    /** @return Full URL using the http scheme. */
    [[nodiscard]] String HttpUrl() const;
    [[nodiscard]] String HttpsUrl() const;
    /** @return Scheme-less normalized representation used for lookups. */
    [[nodiscard]] String ToKey() const;

private:
    explicit Link(Url url);
    Url url_;
};

} // namespace ws
