#pragma once

#include "text.hpp"

#include <optional>

namespace ws::errors {

enum class CrudError {
    kDbFailure,
    kCorruptData,
};

enum class PageTokenError {
    kInvalid,
    kMismatched,
};

enum class CapturePageError {
    kInvalidPageToken,
    kMismatchedPageToken,
    kDbFailure,
};

enum class CreateJobError {
    kDbFailure,
};

enum class CrawlError {
    kFailed,
    kSizeLimit,
    kPersistMetadataFailed,
};

struct [[nodiscard]] CrawlFailure {
    CrawlError code;
    std::optional<String> detail;
};

} // namespace ws::errors
