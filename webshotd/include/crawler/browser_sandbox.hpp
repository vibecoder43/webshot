#pragma once

#include "integers.hpp"
#include "text.hpp"

#include <optional>
#include <string>
#include <vector>

namespace v1::crawler {

struct [[nodiscard]] Geometry {
    i64 width;
    i64 height;
};

struct [[nodiscard]] BrowserSandboxOptions {
    std::string browserBin;
    std::string userDataDir;
    std::string proxyUpstreamSocket;
    std::string cdpSocket;
    std::string netlogPath;
    std::optional<String> geometry;
};

[[nodiscard]] Geometry parseGeometry(const String &value);

[[nodiscard]] std::vector<std::string> buildChromiumArgs(const BrowserSandboxOptions &options);

} // namespace v1::crawler
