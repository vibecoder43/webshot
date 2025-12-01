#pragma once

#include "link.hpp"
#include "text.hpp"

#include <string>
#include <vector>

namespace v1::prefix {

[[nodiscard]] String makePrefixKey(const Link &link);
[[nodiscard]] std::vector<std::string> expandPrefixCandidates(const String &prefixKey);

} // namespace v1::prefix
