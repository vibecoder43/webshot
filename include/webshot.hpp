#pragma once

#include <string>

namespace v1 {

/**
 * @brief Lightweight capture descriptor returned by lookups.
 *
 * A Webshot represents the stored artifact for a single capture. The
 * `location` is a public HTTP URL (or path-style S3 URL) that clients can use
 * to fetch the archived capture (currently stored as a WACZ bundle).
 */
struct [[nodiscard]] Webshot {
    std::string location;
};
}; // namespace v1
