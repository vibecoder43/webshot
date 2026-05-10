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
    kInvalidForwardedHost,
    kInvalidForwardedProto,
};

[[nodiscard]] Expected<Url, StorageUrlError> MakeCaptureDownloadUrl(
    ws::uuid::Uuid uuid, Mode s3_mode, const String &public_base_url,
    const std::optional<String> &request_host, const std::optional<String> &forwarded_host,
    const std::optional<String> &forwarded_proto, bool https_only
);

[[nodiscard]] String StorageUrlErrorMessage(StorageUrlError error);

} // namespace ws
