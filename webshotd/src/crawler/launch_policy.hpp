#pragma once

#include "integers.hpp"

#include <string_view>

namespace v1::crawler {

constexpr std::string_view kBrowserAcceptLanguage = "en";
constexpr auto kProxyUpstreamPort = 3128_i64;
constexpr auto kProxyListenPort = 3128_i64;
constexpr auto kDevtoolsPort = 9222_i64;

} // namespace v1::crawler
