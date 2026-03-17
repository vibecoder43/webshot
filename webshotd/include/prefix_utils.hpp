#pragma once

#include "text.hpp"
#include <string>
namespace v1 {
struct Link;
}

namespace v1::prefix {

[[nodiscard]] String makePrefixKey(const Link &link);
[[nodiscard]] std::string makePrefixTree(const String &prefixKey);

} // namespace v1::prefix
