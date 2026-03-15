#pragma once

#include "integers.hpp"

#include <string_view>

namespace v1::crawler {

constexpr std::string_view kBrowserBin = "chromium";
constexpr std::string_view kBrowserAcceptLanguage = "en";
constexpr auto kProxyUpstreamPort = 3128_i64;
constexpr auto kProxyListenPort = 3128_i64;
constexpr auto kDevtoolsPort = 9222_i64;
constexpr auto kBrowserWindowWidth = 1600_i64;
constexpr auto kBrowserWindowHeight = 900_i64;
constexpr auto kPostLoadDelayMs = 1000_i64;
constexpr auto kNetIdleWaitMs = 0_i64;
constexpr auto kPageExtraDelayMs = 1000_i64;
constexpr auto kBehaviorTimeoutMs = 1000_i64;

} // namespace v1::crawler
