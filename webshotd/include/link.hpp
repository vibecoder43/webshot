#pragma once

#include "url.hpp"

#include <stdexcept>
#include <string>

#include "text.hpp"

namespace v1 {
/**
 * @brief Thrown when a user-supplied URL cannot be normalized or is disallowed.
 */
struct InvalidLinkException : public std::runtime_error {
    using std::runtime_error::runtime_error;
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
 * - Path/query are normalized; query length is limited by the caller.
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
     * enforces a limit on the query part length.
     *
     * @param text Prevalidated, normalized UTF-8 text.
     * @param queryPartLengthMax Maximum allowed length of the query component.
     * @return Normalized Link.
     * @throws InvalidLinkException on parse/validation errors.
     */
    [[nodiscard]] static Link fromText(const String &text, size_t queryPartLengthMax);
    [[nodiscard]] static Link fromTextStripPortQuery(const String &text, size_t queryPartLengthMax);
    [[nodiscard]] static Link fromTextStripPort(const String &text, size_t queryPartLengthMax);

    /** @return Normalized, lower-cased host, punycode if applicable. */
    [[nodiscard]] String host() const;
    /** @return Full URL using the http scheme. */
    [[nodiscard]] String httpUrl() const;
    [[nodiscard]] String httpsUrl() const;
    /** @return Scheme-less normalized representation used for lookups. */
    [[nodiscard]] String normalized() const;
};

} // namespace v1
