#pragma once

#include "integers.hpp"

#include <string_view>

namespace ws::crawler {

constexpr std::string_view kBrowserAcceptLanguage = "en";
constexpr auto kProxyUpstreamPort = 3128_i64;
constexpr auto kProxyListenPort = 3128_i64;
constexpr auto kDevtoolsPort = 9222_i64;

} // namespace ws::crawler
