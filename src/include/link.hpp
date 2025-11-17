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
 * This type encapsulates a normalized representation of a link. It stores the
 * parsed `ada::url_aggregator` and a scheme‑less canonical string used for
 * comparisons and database keys.
 */
struct [[nodiscard]] Link {
    ada::url_aggregator url;
    std::string scheme_less;

    /**
     * @brief Construct a Link from user input.
     *
     * Performs trimming, default scheme insertion (http), validation of scheme
     * and host, punycode handling via ada, clears credentials and fragment, and
     * enforces a limit on the query part length.
     *
     * @param in Raw user input.
     * @param queryPartLengthMax Maximum allowed length of the query component.
     * @return Normalized Link.
     * @throws InvalidLinkException on parse/validation errors.
     */
    [[nodiscard]] static Link fromUserInput(std::string in, size_t queryPartLengthMax);

    /** @return Lower‑cased hostname (punycode if applicable). */
    [[nodiscard]] std::string host() const { return std::string(url.get_hostname()); }
    /** @return Full URL using the http scheme. */
    [[nodiscard]] std::string httpUrl() const { return std::string("http://") + scheme_less; }
    /** @return Scheme‑less normalized representation used for lookups. */
    [[nodiscard]] const std::string &normalized() const { return scheme_less; }
};

} // namespace v1
