#pragma once

#include <string>
#include <vector>

namespace v1::crawler {

[[nodiscard]] std::vector<std::string>
buildChromiumArgs(const std::string &userDataDir, const std::string &netlogPath);

} // namespace v1::crawler
