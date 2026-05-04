#pragma once

#include "expected.hpp"
#include "text.hpp"
#include "url.hpp"

#include <utility>
#include <vector>

namespace ws::s3 {

using ws::Expected;

enum class QueryStringError {
    kInvalidUtf8Key,
    kInvalidUtf8Value,
};

[[nodiscard]] std::optional<Url> ParseUrlWithDefaultHttpScheme(const String &text);
[[nodiscard]] Expected<std::vector<std::pair<String, String>>, QueryStringError>
DecodeQueryString(String search);

} // namespace ws::s3
