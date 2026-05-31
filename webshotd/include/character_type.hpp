#pragma once

#include <absl/strings/ascii.h>

namespace ws::ctype {

// These helpers are ASCII-only.
// Contains exceptions to "just use NumericCast"

[[nodiscard]] inline bool IsAsciiAlpha(char ch) noexcept
{
    return absl::ascii_isalpha(static_cast<unsigned char>(ch));
}

[[nodiscard]] inline bool IsAsciiAlnum(char ch) noexcept
{
    return absl::ascii_isalnum(static_cast<unsigned char>(ch));
}

} // namespace ws::ctype
