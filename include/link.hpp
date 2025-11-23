#pragma once

#include <string>

#include <ada.h>
#include <ada/url_aggregator.h>

namespace v1 {
/**
 * @brief Thrown when a user‑supplied URL cannot be normalized or is disallowed.
 */
struct InvalidLinkException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief Parsed and normalized URL used as an index key.
 *
 * This type encapsulates a normalized representation of a link.
 */
struct [[nodiscard]] Link {
    ada::url_aggregator url;
    std::string schemeLess;

    /**
     * @brief Construct a Link from user input.
     *
     * Performs trimming, default scheme insertion (http), validation of scheme
     * and host, punycode handling, clears credentials and fragment, and
     * enforces a limit on the query part length.
     *
     * @param in Raw user input.
     * @param queryPartLengthMax Maximum allowed length of the query component.
     * @return Normalized Link.
     * @throws InvalidLinkException on parse/validation errors.
     */
    [[nodiscard]] static Link fromUserInput(std::string in, size_t queryPartLengthMax);

    /** @return Normalized, lower‑cased host, punycode if applicable. */
    [[nodiscard]] std::string host() const;
    /** @return Full URL using the http scheme. */
    [[nodiscard]] std::string httpUrl() const;
    /** @return Full URL using the https scheme. */
    [[nodiscard]] std::string httpsUrl() const;
    /** @return Scheme‑less normalized representation used for lookups. */
    [[nodiscard]] std::string normalized() const;
};

} // namespace v1
