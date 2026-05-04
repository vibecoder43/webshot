#pragma once

#include "config.hpp"
#include "expected.hpp"
#include "text.hpp"
#include "url.hpp"
#include "uuid_utils.hpp"

#include <optional>

namespace ws {

enum class StorageUrlError {
    kInvalidPublicBaseUrl,
    kMissingRequestHost,
    kInvalidRequestHost,
};

[[nodiscard]] Expected<Url, StorageUrlError> BuildCaptureDownloadUrl(
    ws::uuid::Uuid uuid, Mode s3_mode, const String &public_base_url,
    const std::optional<String> &request_host
);

[[nodiscard]] String StorageUrlErrorMessage(StorageUrlError error);

} // namespace ws
